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
  // body, so db_emit attributes this to that file's slot
  // (memoized/invalidated with the parse; LSP reads it per-file).
  const Token *t = p_current(p);
  uint32_t start = t ? t->start : 0u;
  uint32_t end = t ? t->byte_end : 0u;
  TinySpan span = span_make_range((uint16_t)p->file.idx, start, end);
  db_emit(p->s, DIAG_ERROR, span, "%s", msg);
}

// -----------------------------------------------------------------------------
// Node Construction
// -----------------------------------------------------------------------------

// Shared helper: push the AST node + the matching span_map entry in
// lockstep. Callers compute the span via whichever entry point fits.
static AstNodeId push_with_span(Parser *p, AstNodeKind kind,
                                uint32_t main_token, AstNodeData data,
                                TinySpan span) {
  AstNodeId id = ast_push_node(p->ast, kind, main_token, data);
  // Stamp the file_id defensively in case the caller built `span`
  // without it (or built TINYSPAN_NONE — defensive clamp from
  // span_make_range underflow path).
  span = span_with_file(span, (uint16_t)p->file.idx);
  *(TinySpan *)vec_push_slot(&p->span_map) = span;
  return id;
}

AstNodeId p_push_node(Parser *p, AstNodeKind kind, uint32_t main_token,
                      AstNodeData data, TinySpan span) {
  return push_with_span(p, kind, main_token, data, span);
}

AstNodeId p_push_node_tok(Parser *p, AstNodeKind kind, uint32_t main_token,
                          uint32_t start_tok_idx, AstNodeData data) {
  // Derive the end token from the current cursor position. Clamp to
  // never go below start_tok_idx — this is the safety guarantee that
  // makes span construction immune to parser-recovery cursor states.
  //
  // Token count assumed > 0 (lex always emits at least EOF). start_tok_idx
  // is bounded by the caller (captured from a valid p->pos at function
  // entry). end_tok_idx is bounded by p->pos which is monotonic up to
  // tokens.count.
  uint32_t pos = p->pos;
  uint32_t end_tok_idx = (pos > start_tok_idx) ? pos - 1 : start_tok_idx;
  const Token *st = (const Token *)vec_get((Vec *)p->tokens, start_tok_idx);
  const Token *et = (const Token *)vec_get((Vec *)p->tokens, end_tok_idx);
  TinySpan span = span_make_range((uint16_t)p->file.idx, st->start, et->byte_end);
  return push_with_span(p, kind, main_token, data, span);
}

// -----------------------------------------------------------------------------
// Parent-edge population.
//
// Walks the ASTStore once linearly and records `parents[child.idx] = id`
// for every parent→child edge via ast_visit_children (the shared
// kind→children dispatch). Single forward pass works because children
// are always emitted before their parents (post-order via p_push_node),
// so child.idx < parent.idx unconditionally.
//
// The caller has already memset(parents, 0, ...); ast_visit_children
// only emits non-zero child ids, so module root + sentinel correctly
// stay parent=0.
// -----------------------------------------------------------------------------
typedef struct {
  AstNodeId *parents;
  uint32_t parent_id;
} PopulateParentsCtx;

static void populate_parents_cb(AstNodeId child, void *ctx) {
  PopulateParentsCtx *pc = (PopulateParentsCtx *)ctx;
  pc->parents[child.idx].idx = pc->parent_id;
}

static void populate_parents(const ASTStore *ast, AstNodeId *parents,
                             uint32_t count) {
  PopulateParentsCtx ctx = {.parents = parents, .parent_id = 0};
  for (uint32_t id = 1; id < count; id++) {
    ctx.parent_id = id;
    ast_visit_children(ast, (AstNodeId){id}, populate_parents_cb, &ctx);
  }
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

  // All malloc-backed (growable, no caps). span_map, top_level_index and
  // scratch are all transient — vec_free'd at the end of this function
  // (top_level_index is flattened into a per-file-arena FileArray).
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
  // top_level_index → a FileArray in the per-file arena (flattened, like
  // line_starts / trivia). The local Vec is transient — freed below.
  {
    uint32_t tn = (uint32_t)p.top_level_index.count;
    FileArray tfa = {.data = NULL, .count = tn};
    if (tn) {
      tfa.data = arena_alloc(ma, (size_t)tn * sizeof(TopLevelEntry));
      memcpy(tfa.data, p.top_level_index.data,
             (size_t)tn * sizeof(TopLevelEntry));
    }
    *(FileArray *)vec_get(&s->files.top_level_indices, f) = tfa;
  }

  // Flatten span_map (+ zeroed parent/defs maps — later passes fill
  // those) into one contiguous FileNodeData block in the per-file
  // arena. Block layout per node: 8 (span) + 4 (parent) + 4 (def) =
  // 16 bytes/node. node_count includes the sentinel at index 0.
  // (types[] was removed in the Option-C migration — per-node types
  //  now live in db.node_types_pool, addressed by per-decl ranges.)
  uint32_t node_count = (uint32_t)p.span_map.count;
  void *block = arena_alloc(ma, (size_t)node_count * 16);
  TinySpan *spans = (TinySpan *)block;
  AstNodeId *parents = (AstNodeId *)((uint8_t *)block + (size_t)node_count * 8);
  DefId *defs = (DefId *)((uint8_t *)parents + (size_t)node_count * 4);

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
    memset(defs, 0, (size_t)node_count * sizeof(DefId));
    populate_parents(p.ast, parents, node_count);
  }

  FileNodeData *nd = (FileNodeData *)vec_get(&s->files.node_data, f);
  nd->spans = spans;
  nd->parents = parents;
  nd->defs = defs;
  *(uint32_t *)vec_get(&s->files.node_counts, f) = node_count;

  // AstIdMap — top-level granularity. Allocated in the per-file arena
  // (auto-reclaimed by the query body's arena_reset on reparse). The
  // db column is `Vec<void*>` to keep the column-init layer free of
  // AstIdMap's transitive includes.
  struct AstIdMap *id_map = build_ast_id_map(&p.top_level_index, p.ast, ma);
  *(struct AstIdMap **)vec_get(&s->files.ast_id_maps, f) = id_map;

  // Transient malloc buffers: span_map was flattened into the arena
  // block, top_level_index into a per-file-arena FileArray, scratch is
  // parse-local. Free them now. (The ast Vecs persist into columns and
  // are freed by the query body on the NEXT reparse, before arena_reset.)
  vec_free(&p.span_map);
  vec_free(&p.top_level_index);
  vec_free(&p.scratch);
}
