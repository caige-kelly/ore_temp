#include "parser.h"

#include "../db/diag/diag.h"
#include "../db/workspace/ast_id_map.h"

#include <string.h>

// -----------------------------------------------------------------------------
// Cursor Primitives
// -----------------------------------------------------------------------------

bool p_is_eof(const Parser *p) { return p_peek(p) == TK_EOF; }

const Token *p_current(const Parser *p) {
  // Typed unchecked read: the index is range-clamped right here, so
  // vec_get's bounds check + call overhead is pure redundancy on this
  // hot path (every p_peek/p_advance/p_consume/p_is_eof routes here).
  const Token *toks = (const Token *)p->tokens->data;
  if (p->pos >= p->tokens->count) {
    if (p->tokens->count == 0)
      return NULL;
    return &toks[p->tokens->count - 1];
  }
  return &toks[p->pos];
}

TokenKind p_peek(const Parser *p) {
  const Token *t = p_current(p);
  return t ? t->kind : TK_EOF;
}

TokenKind p_peek_at(const Parser *p, uint32_t offset) {
  uint32_t idx = p->pos + offset;
  if (idx >= p->tokens->count) {
    if (p->tokens->count == 0)
      return TK_EOF;
    const Token *last = vec_get((Vec *)p->tokens, p->tokens->count - 1);
    return last->kind;
  }
  const Token *t = vec_get((Vec *)p->tokens, idx);
  return t->kind;
}

const Token *p_advance(Parser *p) {
  const Token *t = p_current(p);
  if (!p_is_eof(p)) {
    p->pos++;
  }
  return t;
}

bool p_match(Parser *p, TokenKind kind) {
  if (p_peek(p) == kind) {
    p_advance(p);
    return true;
  }
  return false;
}

const Token *p_consume(Parser *p, TokenKind kind, const char *err_msg) {
  if (p_peek(p) == kind) {
    return p_advance(p);
  }
  p_error(p, err_msg);
  return NULL;
}

void p_error(Parser *p, const char *msg) {
  // Slot-keyed diagnostic. The parser runs inside QUERY_FILE_AST's
  // body, so db_diag_error attributes this to that file's slot
  // (memoized/invalidated with the parse; LSP reads it per-file).
  const Token *t = p_current(p);
  uint32_t start = t ? t->start : 0u;
  uint32_t end = t ? t->byte_end : 0u;
  TinySpan span = span_make_range((uint16_t)p->file.idx, start, end);
  db_diag_error(p->s, span, msg);
}

// -----------------------------------------------------------------------------
// Node Construction
// -----------------------------------------------------------------------------

AstNodeId p_push_node(Parser *p, AstNodeKind kind, uint32_t main_token,
                      AstNodeData data, TinySpan span) {
  AstNodeId id = ast_push_node(p->ast, kind, main_token, data);

  // span_map parallels the ASTStore by node index. Stamp the file_id
  // defensively in case the caller built `span` without it.
  span = span_with_file(span, (uint16_t)p->file.idx);
  *(TinySpan *)vec_push_slot(&p->span_map) = span;

  return id;
}

// -----------------------------------------------------------------------------
// Parent-edge population.
//
// Walks the ASTStore once linearly and records `parents[child.idx] = id`
// for every parent→child edge. Works as a single forward pass because
// children are always emitted before their parents (post-order via
// p_push_node), so child.idx < parent.idx unconditionally. Per-kind
// dispatch mirrors the dumper's child enumeration in ast_dump_inc.h —
// any drift would silently miss edges, so keep them in lockstep.
//
// The caller has already memset(parents, 0, ...); we only write the
// non-zero edges. Module root + sentinel correctly stay parent=0.
// -----------------------------------------------------------------------------
static void populate_parents(const ASTStore *ast, AstNodeId *parents,
                             uint32_t count) {
  const AstNodeKind *kinds = (const AstNodeKind *)ast->kinds.data;
  const AstNodeData *data  = (const AstNodeData  *)ast->data.data;
  const uint32_t    *extra = (const uint32_t    *)ast->extra.data;

#define SET_PARENT(child_idx)                                                  \
  do {                                                                         \
    uint32_t _c = (child_idx);                                                 \
    if (_c != 0)                                                               \
      parents[_c].idx = id;                                                    \
  } while (0)

  for (uint32_t id = 1; id < count; id++) {
    AstNodeKind k = kinds[id];
    AstNodeData d = data[id];

    // single_child group (matches ast_dump_inc.h):
    // statement wrappers, type-position prefix unaries, and the full
    // unary value family (NEG..DEERR).
    if (k == AST_STMT_RETURN || k == AST_STMT_DEFER ||
        k == AST_TYPE_PTR || k == AST_TYPE_SLICE || k == AST_TYPE_MANYPTR ||
        k == AST_TYPE_OPTIONAL || k == AST_TYPE_CONST ||
        (k >= AST_EXPR_UNARY_NEG && k <= AST_EXPR_UNARY_DEERR)) {
      SET_PARENT(d.single_child.idx);
      continue;
    }

    // bin: lhs + rhs (binary ops, assigns, FIELD, INDEX, HANDLE, MASK).
    if ((k >= AST_EXPR_BIN_ADD && k <= AST_EXPR_BIN_SHR) ||
        (k >= AST_EXPR_ASSIGN && k <= AST_EXPR_ASSIGN_BIT_XOR) ||
        k == AST_EXPR_FIELD || k == AST_EXPR_INDEX ||
        k == AST_EXPR_HANDLE || k == AST_EXPR_MASK) {
      SET_PARENT(d.bin.lhs.idx);
      SET_PARENT(d.bin.rhs.idx);
      continue;
    }

    // Pure leaves (no children) — explicit no-op group.
    if (k == AST_EXPR_PATH || k == AST_EXPR_LIT_INT ||
        k == AST_EXPR_LIT_FLOAT || k == AST_EXPR_LIT_STRING ||
        k == AST_EXPR_LIT_BYTE || k == AST_EXPR_LIT_BOOL ||
        k == AST_EXPR_LIT_NIL || k == AST_EXPR_ASM ||
        k == AST_EXPR_WILDCARD || k == AST_EXPR_ENUM_REF ||
        k == AST_STMT_BREAK || k == AST_STMT_CONTINUE ||
        k == AST_TYPE_VOID || k == AST_TYPE_NORETURN ||
        k == AST_TYPE_ANYTYPE || k == AST_TYPE_TYPE ||
        k == AST_ERROR) {
      continue;
    }

    // extras-based kinds: layout depends on kind.
    const uint32_t *ex = &extra[d.extra_idx.idx];
    switch ((int)k) {
    case AST_TYPE_ARRAY: // [size, elem]
      SET_PARENT(ex[0]);
      SET_PARENT(ex[1]);
      break;

    case AST_EXPR_PRODUCT: { // [type, fcount, f0...]
      SET_PARENT(ex[0]);
      uint32_t fc = ex[1];
      for (uint32_t i = 0; i < fc; i++)
        SET_PARENT(ex[2 + i]);
      break;
    }

    case AST_DECL_CONST: // [name_strid, type, value, meta]
    case AST_DECL_VAR:
      SET_PARENT(ex[1]); // type
      SET_PARENT(ex[2]); // value
      break;

    case AST_DECL_DESTRUCTURE: // [pattern, type, value, meta]
      SET_PARENT(ex[0]);
      SET_PARENT(ex[1]);
      SET_PARENT(ex[2]);
      break;

    case AST_EXPR_LAMBDA: { // [ret_type, body, effect, pcount, p0...]
      SET_PARENT(ex[0]);
      SET_PARENT(ex[1]);
      SET_PARENT(ex[2]);
      uint32_t pc = ex[3];
      for (uint32_t i = 0; i < pc; i++)
        SET_PARENT(ex[4 + i]);
      break;
    }

    case AST_TYPE_FN: { // [ret, effect, pcount, p0...]
      SET_PARENT(ex[0]);
      SET_PARENT(ex[1]);
      uint32_t pc = ex[2];
      for (uint32_t i = 0; i < pc; i++)
        SET_PARENT(ex[3 + i]);
      break;
    }

    case AST_STMT_BLOCK: { // [stmt_count, s0...]
      uint32_t sc = ex[0];
      for (uint32_t i = 0; i < sc; i++)
        SET_PARENT(ex[1 + i]);
      break;
    }

    case AST_STMT_IF: // [cond, then, else]
      SET_PARENT(ex[0]);
      SET_PARENT(ex[1]);
      SET_PARENT(ex[2]);
      break;

    case AST_STMT_SWITCH: { // [scrutinee, arm_count, arm0...]
      SET_PARENT(ex[0]);
      uint32_t ac = ex[1];
      for (uint32_t i = 0; i < ac; i++)
        SET_PARENT(ex[2 + i]);
      break;
    }

    case AST_STMT_SWITCH_ARM: { // [pat_count, pat0..N, body]
      uint32_t pc = ex[0];
      for (uint32_t i = 0; i < pc; i++)
        SET_PARENT(ex[1 + i]);
      SET_PARENT(ex[1 + pc]); // body
      break;
    }

    case AST_EXPR_BUILTIN: { // [name_strid, arg_count, arg0...]
      uint32_t argc = ex[1];
      for (uint32_t i = 0; i < argc; i++)
        SET_PARENT(ex[2 + i]);
      break;
    }

    case AST_STMT_LOOP: // [label_strid, init, cond, step, body]
      SET_PARENT(ex[1]);
      SET_PARENT(ex[2]);
      SET_PARENT(ex[3]);
      SET_PARENT(ex[4]);
      break;

    case AST_DECL_MODULE: // [count, c0...]
    case AST_DECL_STRUCT:
    case AST_DECL_UNION:
    case AST_DECL_ENUM: {
      uint32_t c = ex[0];
      for (uint32_t i = 0; i < c; i++)
        SET_PARENT(ex[1 + i]);
      break;
    }

    case AST_DECL_PARAM: // [name, type, is_comptime]
      SET_PARENT(ex[0]);
      SET_PARENT(ex[1]);
      break;

    case AST_DECL_FIELD: // [name, type, vis, fpos]
      SET_PARENT(ex[0]);
      SET_PARENT(ex[1]);
      SET_PARENT(ex[3]); // fpos is a node id
      break;

    case AST_DECL_VARIANT: // [name, value]
    case AST_INIT_FIELD:
      SET_PARENT(ex[0]);
      SET_PARENT(ex[1]);
      break;

    case AST_EXPR_CALL: { // [callee, arg_count, arg0...]
      SET_PARENT(ex[0]);
      uint32_t argc = ex[1];
      for (uint32_t i = 0; i < argc; i++)
        SET_PARENT(ex[2 + i]);
      break;
    }

    case AST_EXPR_SLICE: // [recv, lo, hi]
      SET_PARENT(ex[0]);
      SET_PARENT(ex[1]);
      SET_PARENT(ex[2]);
      break;

    case AST_EXPR_HANDLER: { // [hdr, effect, initially, return, finally,
                             //  branch_count, (sort, name_tok, lambda)xN]
      SET_PARENT(ex[1]);
      SET_PARENT(ex[2]);
      SET_PARENT(ex[3]);
      SET_PARENT(ex[4]);
      uint32_t bc = ex[5];
      for (uint32_t i = 0; i < bc; i++)
        SET_PARENT(ex[6 + i * 3 + 2]); // lambda (slots 0/1 are sort/name_tok)
      break;
    }

    case AST_EXPR_EFFECT_ROW: { // [flags, tail_strid, label_count, l0...]
      uint32_t lc = ex[2];
      for (uint32_t i = 0; i < lc; i++)
        SET_PARENT(ex[3 + i]);
      break;
    }

    case AST_DECL_EFFECT: { // [hdr, in_type, tparam_count, tp..,
                            //  sig_count, (sort, name_tok, sig)xN]
      SET_PARENT(ex[1]); // in_type
      uint32_t tpc = ex[2];
      for (uint32_t i = 0; i < tpc; i++)
        SET_PARENT(ex[3 + i]);
      uint32_t sc_at = 3 + tpc;
      uint32_t sc = ex[sc_at];
      for (uint32_t i = 0; i < sc; i++)
        SET_PARENT(ex[sc_at + 1 + i * 3 + 2]); // sig lambda
      break;
    }

    default:
      // Any kind not enumerated here gets parent=0 for its children
      // (the memset baseline). Add a case if you add a new kind with
      // children, OR if dumper coverage grows.
      break;
    }
  }

#undef SET_PARENT
}

// -----------------------------------------------------------------------------
// AstIdMap build — top-level granularity.
//
// Populates an AstIdMap from the parser's TopLevelEntry vector so sema
// can later resolve `AstId → AstNodeId` for incremental reparses. Uses
// `ast_id_map_insert`, which (post-unification) shares the canonical
// `ast_id_compute` hash with `parse_top_level_decls` — so the returned
// AstId matches `entry.ast_id` slot-for-slot.
//
// Granularity is top-level-only; sema can extend later for nested decls
// (struct fields, enum variants, params) when it actually needs them.
// -----------------------------------------------------------------------------
static struct AstIdMap *build_ast_id_map(const Vec *top_level_index,
                                         const ASTStore *ast, Arena *arena) {
  struct AstIdMap *map = arena_alloc(arena, sizeof(struct AstIdMap));
  ast_id_map_init(map, arena);

  const AstNodeKind *kinds = (const AstNodeKind *)ast->kinds.data;
  for (size_t i = 0; i < top_level_index->count; i++) {
    const TopLevelEntry *e = vec_get((Vec *)top_level_index, i);
    if (e->node.idx == 0)
      continue; // skip sentinel / unresolvable
    AstNodeKind k = kinds[e->node.idx];
    ast_id_map_insert(map, (uint32_t)k, e->name, e->node);
  }

  return map;
}

// -----------------------------------------------------------------------------
// Core Driver — QUERY_FILE_AST body
// -----------------------------------------------------------------------------

void parse_file(struct db *s, FileId fid, const Vec *tokens) {
  uint32_t f = file_id_local(fid);
  Arena *ma = (Arena *)vec_get(&s->files.arenas, f);

  // The ASTStore STRUCT lives in the per-file arena (fixed size,
  // reclaimed by arena_reset on reparse). Its Vecs are malloc-backed
  // and growable (ast_store_init) — freed explicitly by the query
  // body before the next arena_reset.
  // Node-count estimate for pre-sizing the growable AST/side Vecs.
  // tokens->count is exact & free here; node count is ~1–1.5x tokens
  // (leaf node per token + fewer structural nodes). A soft reserve
  // (vec_grow) — never a cap — eliminates the realloc+memmove doubling
  // storm (measured ~16% self-time pre-fix). Empty file → ~0, vec_grow
  // floors at 8; sentinels still fit.
  size_t node_est = tokens ? tokens->count : 0;

  ASTStore *ast = arena_alloc(ma, sizeof(ASTStore));
  ast_store_init(ast, ma, node_est);

  Parser p = {
      .s = s,
      .file = fid,
      .tokens = tokens,
      .pos = 0,
      .ast = ast,
      .parsing_type = false,
  };

  // All malloc-backed (growable, no caps). span_map + scratch are
  // transient — vec_free'd at the end of this function. top_level_index
  // persists into the db column; the query body vec_free's the prior
  // one on reparse before calling here.
  vec_init(&p.span_map, sizeof(TinySpan));
  vec_init(&p.scratch, sizeof(uint32_t));
  vec_init(&p.top_level_index, sizeof(TopLevelEntry));

  // span_map is 1:1 with AST nodes — pre-size on the same estimate
  // (same soft-reserve rationale as ast_store_init).
  if (node_est > 0) {
    vec_grow(&p.span_map, node_est);
  }

  // Sentinel keeps span_map index-aligned with the ASTStore's node 0
  // (AST_ERROR sentinel pushed by ast_store_init).
  TinySpan dummy_span = {0};
  vec_push(&p.span_map, &dummy_span);

  extern void parse_top_level_decls(Parser * p);
  parse_top_level_decls(&p);

  // Commit parser outputs to the file's columns.
  *(void **)vec_get(&s->files.asts, f) = p.ast;
  *(Vec *)vec_get(&s->files.top_level_indices, f) = p.top_level_index;

  // Flatten span_map (+ zeroed parent/type maps — later passes fill
  // those) into one contiguous ModuleNodeData block in the per-file
  // arena. node_count includes the sentinel at index 0.
  uint32_t node_count = (uint32_t)p.span_map.count;
  void *block = arena_alloc(ma, (size_t)node_count * 16);
  TinySpan *spans = (TinySpan *)block;
  AstNodeId *parents = (AstNodeId *)((uint8_t *)block + (size_t)node_count * 8);
  uint32_t *types = (uint32_t *)((uint8_t *)parents + (size_t)node_count * 4);

  if (node_count > 0 && p.span_map.data) {
    memcpy(spans, p.span_map.data, (size_t)node_count * sizeof(TinySpan));
  } else if (node_count > 0) {
    memset(spans, 0, (size_t)node_count * sizeof(TinySpan));
  }
  if (node_count > 0) {
    // parents: zero baseline; populate_parents below fills the real
    // edges. Any kind not enumerated by the walker keeps parent=0,
    // which is correct for the module root + AST_ERROR sentinel.
    memset(parents, 0, (size_t)node_count * sizeof(AstNodeId));
    memset(types, 0, (size_t)node_count * sizeof(uint32_t));
    populate_parents(p.ast, parents, node_count);
  }

  ModuleNodeData *nd = (ModuleNodeData *)vec_get(&s->files.node_data, f);
  nd->spans = spans;
  nd->parents = parents;
  nd->types = (IpIndex *)types;
  *(uint32_t *)vec_get(&s->files.node_counts, f) = node_count;

  // AstIdMap — top-level granularity. Allocated in the per-file arena
  // (auto-reclaimed by the query body's arena_reset on reparse). The
  // db column is `Vec<void*>` to keep the column-init layer free of
  // AstIdMap's transitive includes.
  struct AstIdMap *id_map =
      build_ast_id_map(&p.top_level_index, p.ast, ma);
  *(struct AstIdMap **)vec_get(&s->files.ast_id_maps, f) = id_map;

  // Transient malloc buffers: span_map was just flattened into the
  // arena block; scratch is parse-local. Free them now. (ast Vecs +
  // top_level_index persist into columns and are freed by the query
  // body on the NEXT reparse, before arena_reset.)
  vec_free(&p.span_map);
  vec_free(&p.scratch);
}
