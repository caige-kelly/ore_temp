#include "ast.h"

#include <assert.h>

void ast_store_init(ASTStore *ast, Arena *arena, size_t max_nodes) {
  (void)arena;
  // Malloc-backed, growable: AST size is NOT bounded by token count
  // (sentinel + module node + synthetics + count-prefixed extras all
  // exceed it; empty files would otherwise assert). The ASTStore
  // STRUCT lives in the per-module arena; these Vecs are freed
  // explicitly on reparse before that arena is reset.
  ast->arena = NULL;
  vec_init(&ast->kinds, sizeof(AstNodeKind));
  vec_init(&ast->main_tokens, sizeof(uint32_t));
  vec_init(&ast->data, sizeof(AstNodeData));
  vec_init(&ast->extra, sizeof(uint32_t));

  // Pre-size from a node-count estimate (token count, passed by
  // parse_file). vec_grow is a SOFT reserve ("grow to at least") — not
  // a cap — so an over/under estimate only changes regrowth count,
  // never correctness. Collapses the ~21 doublings (cap 0→…→~1M, each
  // a realloc + _platform_memmove of all prior contents — the measured
  // #1 buffer-copy cost) down to ~0–1.
  if (max_nodes > 0) {
    vec_grow(&ast->kinds, max_nodes);
    vec_grow(&ast->main_tokens, max_nodes);
    vec_grow(&ast->data, max_nodes);
    vec_grow(&ast->extra, max_nodes);
  }

  // Push sentinel node 0
  AstNodeKind dummy_kind = AST_ERROR;
  uint32_t dummy_token = 0;
  AstNodeData dummy_data = {0};

  vec_push(&ast->kinds, &dummy_kind);
  vec_push(&ast->main_tokens, &dummy_token);
  vec_push(&ast->data, &dummy_data);
}

AstNodeId ast_push_node(ASTStore *ast, AstNodeKind kind, uint32_t main_token,
                        AstNodeData data) {
  uint32_t idx = ast->kinds.count;

  // Typed compile-time-constant-size stores (no per-element
  // _platform_memmove call — see vec_push_slot). Hot: ~3 per AST node.
  *(AstNodeKind *)vec_push_slot(&ast->kinds) = kind;
  *(uint32_t *)vec_push_slot(&ast->main_tokens) = main_token;
  *(AstNodeData *)vec_push_slot(&ast->data) = data;

  return (AstNodeId){.idx = idx};
}

AstExtraDataIdx ast_push_extra(ASTStore *ast, const uint32_t *items,
                               uint32_t count) {
  uint32_t idx = ast->extra.count;
  // One grow + one block copy instead of `count` vec_push calls.
  vec_append_n(&ast->extra, items, count);
  return (AstExtraDataIdx){.idx = idx};
}

// Single source of truth for "what node ids does each kind reference."
// Mirrors the dumper's per-kind dispatch (ast_dump_inc.h) and the
// previous inline dispatch in parser.c's populate_parents — both can
// now route through this helper. Non-node slots (StrIds, raw token
// indices, flag/meta u32s, fpos that's an expr id, etc.) are filtered
// here so callers don't need per-slot knowledge.
void ast_visit_children(const ASTStore *ast, AstNodeId id, AstChildFn fn,
                        void *ctx) {
  if (id.idx == 0 || id.idx >= ast->kinds.count)
    return;
  AstNodeKind k = ((const AstNodeKind *)ast->kinds.data)[id.idx];
  AstNodeData d = ((const AstNodeData *)ast->data.data)[id.idx];
  const uint32_t *extra = (const uint32_t *)ast->extra.data;
  const uint32_t *ex = &extra[d.extra_idx.idx];

#define EMIT(child_idx)                                                        \
  do {                                                                         \
    AstNodeId _c = {(child_idx)};                                              \
    if (_c.idx != 0)                                                           \
      fn(_c, ctx);                                                             \
  } while (0)

  // Single switch over every AstNodeKind. -O2 generates a jump table
  // (kinds are densely-packed u8 values). This consolidates what used
  // to be three sequential range-tests-then-switch into a single
  // jump-table dispatch — hot-path win when this walker runs ~1M times
  // per parse (post-parse parents fill).
  switch (k) {
  // Leaves (no children).
  case AST_ERROR:
  case AST_EXPR_PATH:
  case AST_EXPR_LIT_INT:
  case AST_EXPR_LIT_FLOAT:
  case AST_EXPR_LIT_STRING:
  case AST_EXPR_LIT_BYTE:
  case AST_EXPR_LIT_BOOL:
  case AST_EXPR_LIT_NIL:
  case AST_EXPR_ASM:
  case AST_EXPR_WILDCARD:
  case AST_EXPR_ENUM_REF:
  case AST_STMT_BREAK:
  case AST_STMT_CONTINUE:
    return;

  // single_child: statement wrappers, type-position prefix unaries,
  // full unary value family.
  case AST_STMT_RETURN:
  case AST_STMT_DEFER:
  case AST_TYPE_PTR:
  case AST_TYPE_SLICE:
  case AST_TYPE_MANYPTR:
  case AST_TYPE_OPTIONAL:
  case AST_TYPE_CONST:
  case AST_EXPR_UNARY_NEG:
  case AST_EXPR_UNARY_NOT:
  case AST_EXPR_UNARY_BIT_NOT:
  case AST_EXPR_UNARY_REF:
  case AST_EXPR_UNARY_DEREF:
  case AST_EXPR_UNARY_INC:
  case AST_EXPR_UNARY_DENIL:
  case AST_EXPR_UNARY_DEERR:
    EMIT(d.single_child.idx);
    return;

  // bin: lhs + rhs (binary ops, assigns, FIELD, INDEX, HANDLE, MASK).
  case AST_EXPR_BIN_ADD:
  case AST_EXPR_BIN_SUB:
  case AST_EXPR_BIN_MUL:
  case AST_EXPR_BIN_DIV:
  case AST_EXPR_BIN_MOD:
  case AST_EXPR_BIN_POW:
  case AST_EXPR_BIN_EQ:
  case AST_EXPR_BIN_NEQ:
  case AST_EXPR_BIN_LT:
  case AST_EXPR_BIN_LE:
  case AST_EXPR_BIN_GT:
  case AST_EXPR_BIN_GE:
  case AST_EXPR_BIN_AND:
  case AST_EXPR_BIN_OR:
  case AST_EXPR_BIN_ORELSE:
  case AST_EXPR_BIN_CATCH:
  case AST_EXPR_BIN_BIT_AND:
  case AST_EXPR_BIN_BIT_OR:
  case AST_EXPR_BIN_BIT_XOR:
  case AST_EXPR_BIN_SHL:
  case AST_EXPR_BIN_SHR:
  case AST_EXPR_ASSIGN:
  case AST_EXPR_ASSIGN_ADD:
  case AST_EXPR_ASSIGN_SUB:
  case AST_EXPR_ASSIGN_MUL:
  case AST_EXPR_ASSIGN_DIV:
  case AST_EXPR_ASSIGN_MOD:
  case AST_EXPR_ASSIGN_BIT_AND:
  case AST_EXPR_ASSIGN_BIT_OR:
  case AST_EXPR_ASSIGN_BIT_XOR:
  case AST_EXPR_FIELD:
  case AST_EXPR_INDEX:
  case AST_EXPR_HANDLE:
  case AST_EXPR_MASK:
    EMIT(d.bin.lhs.idx);
    EMIT(d.bin.rhs.idx);
    return;

  // extras-based kinds.
  case AST_TYPE_ARRAY: // [size, elem]
    EMIT(ex[0]);
    EMIT(ex[1]);
    return;

  case AST_EXPR_PRODUCT: { // [type, fcount, f0...]
    EMIT(ex[0]);
    uint32_t fc = ex[1];
    for (uint32_t i = 0; i < fc; i++)
      EMIT(ex[2 + i]);
    return;
  }

  case AST_DECL_CONST: // [name_strid, type, value, meta]
  case AST_DECL_VAR:
    EMIT(ex[1]);
    EMIT(ex[2]);
    return;

  case AST_DECL_DESTRUCTURE: // [pattern, type, value, meta]
    EMIT(ex[0]);
    EMIT(ex[1]);
    EMIT(ex[2]);
    return;

  case AST_EXPR_LAMBDA: { // [ret_type, body, effect, pcount, p0...]
    EMIT(ex[0]);
    EMIT(ex[1]);
    EMIT(ex[2]);
    uint32_t pc = ex[3];
    for (uint32_t i = 0; i < pc; i++)
      EMIT(ex[4 + i]);
    return;
  }

  case AST_TYPE_FN: { // [ret, effect, pcount, p0...]
    EMIT(ex[0]);
    EMIT(ex[1]);
    uint32_t pc = ex[2];
    for (uint32_t i = 0; i < pc; i++)
      EMIT(ex[3 + i]);
    return;
  }

  case AST_STMT_BLOCK: { // [stmt_count, s0...]
    uint32_t sc = ex[0];
    for (uint32_t i = 0; i < sc; i++)
      EMIT(ex[1 + i]);
    return;
  }

  case AST_STMT_IF: // [cond, then, else]
    EMIT(ex[0]);
    EMIT(ex[1]);
    EMIT(ex[2]);
    return;

  case AST_STMT_SWITCH: { // [scrutinee, arm_count, arm0...]
    EMIT(ex[0]);
    uint32_t ac = ex[1];
    for (uint32_t i = 0; i < ac; i++)
      EMIT(ex[2 + i]);
    return;
  }

  case AST_STMT_SWITCH_ARM: { // [pat_count, pat0..N, body]
    uint32_t pc = ex[0];
    for (uint32_t i = 0; i < pc; i++)
      EMIT(ex[1 + i]);
    EMIT(ex[1 + pc]);
    return;
  }

  case AST_EXPR_BUILTIN: { // [name_strid, arg_count, arg0...]
    uint32_t argc = ex[1];
    for (uint32_t i = 0; i < argc; i++)
      EMIT(ex[2 + i]);
    return;
  }

  case AST_STMT_LOOP: // [label_strid, init, cond, step, body]
    EMIT(ex[1]);
    EMIT(ex[2]);
    EMIT(ex[3]);
    EMIT(ex[4]);
    return;

  case AST_DECL_MODULE: // [count, c0...]
  case AST_DECL_STRUCT:
  case AST_DECL_UNION:
  case AST_DECL_ENUM: {
    uint32_t c = ex[0];
    for (uint32_t i = 0; i < c; i++)
      EMIT(ex[1 + i]);
    return;
  }

  case AST_DECL_PARAM: // [name_strid, type, is_comptime]
    // ex[0] is a StrId, not an AstNodeId — don't visit it as a child.
    EMIT(ex[1]);
    return;

  case AST_DECL_FIELD: // [name_strid, type, vis, fpos]
    // ex[0] is a StrId, not an AstNodeId.
    EMIT(ex[1]);
    EMIT(ex[3]);
    return;

  case AST_DECL_VARIANT: // [name_strid, value]
  case AST_INIT_FIELD:   // [name_strid (0=positional), value]
    // ex[0] is a StrId, not an AstNodeId.
    EMIT(ex[1]);
    return;

  case AST_EXPR_CALL: { // [callee, arg_count, arg0...]
    EMIT(ex[0]);
    uint32_t argc = ex[1];
    for (uint32_t i = 0; i < argc; i++)
      EMIT(ex[2 + i]);
    return;
  }

  case AST_EXPR_SLICE: // [recv, lo, hi]
    EMIT(ex[0]);
    EMIT(ex[1]);
    EMIT(ex[2]);
    return;

  case AST_EXPR_HANDLER: { // [hdr, effect, initially, return, finally,
                           //  branch_count, (sort, name_tok, lambda)xN]
    EMIT(ex[1]);
    EMIT(ex[2]);
    EMIT(ex[3]);
    EMIT(ex[4]);
    uint32_t bc = ex[5];
    for (uint32_t i = 0; i < bc; i++)
      EMIT(ex[6 + i * 3 + 2]);
    return;
  }

  case AST_EXPR_EFFECT_ROW: { // [flags, tail_strid, label_count, l0...]
    uint32_t lc = ex[2];
    for (uint32_t i = 0; i < lc; i++)
      EMIT(ex[3 + i]);
    return;
  }

  case AST_DECL_EFFECT: { // [hdr, in_type, tparam_count, tp..,
                          //  sig_count, (sort, name_tok, sig)xN]
    EMIT(ex[1]);
    uint32_t tpc = ex[2];
    for (uint32_t i = 0; i < tpc; i++)
      EMIT(ex[3 + i]);
    uint32_t sc_at = 3 + tpc;
    uint32_t sc = ex[sc_at];
    for (uint32_t i = 0; i < sc; i++)
      EMIT(ex[sc_at + 1 + i * 3 + 2]);
    return;
  }
  }
#undef EMIT
}

// =========================================================================
// Structural subtree fingerprint (see ast.h). Position-independent hash
// of one top-level decl's AST subtree, for QUERY_DECL_AST early-cutoff.
// =========================================================================

// FNV-1a 64 — a change-detector, not crypto; fold each u32 byte-wise.
#define AST_FP_BASIS 0xcbf29ce484222325ULL
#define AST_FP_PRIME 0x100000001b3ULL
static inline uint64_t fp_u32(uint64_t h, uint32_t v) {
  for (int i = 0; i < 4; i++) {
    h ^= (uint8_t)(v >> (i * 8));
    h *= AST_FP_PRIME;
  }
  return h;
}
static inline uint64_t fp_u64(uint64_t h, uint64_t v) {
  return fp_u32(fp_u32(h, (uint32_t)v), (uint32_t)(v >> 32));
}

// Relativize a child node-id against the subtree minimum: NONE (0) stays
// 0; a real id becomes (id - node_min) + 1, so "no child" and "child at
// node_min" hash distinctly. A real subtree child is always >= node_min.
static inline uint32_t fp_rel_node(uint32_t v, uint32_t node_min) {
  return v ? (v - node_min) + 1u : 0u;
}

// Subtree extent — min node id, min main_token, node count. Recursive
// via the visit callback; depth == AST nesting (the parser already
// recursed exactly this deep to build these nodes).
struct ast_extent {
  const ASTStore *ast;
  uint32_t node_min;
  uint32_t tok_min;
  uint32_t count;
};
static void ast_extent_cb(AstNodeId child, void *ctxv) {
  struct ast_extent *e = (struct ast_extent *)ctxv;
  if (child.idx < e->node_min)
    e->node_min = child.idx;
  uint32_t mt = ((const uint32_t *)e->ast->main_tokens.data)[child.idx];
  if (mt < e->tok_min)
    e->tok_min = mt;
  e->count++;
  ast_visit_children(e->ast, child, ast_extent_cb, e);
}

// Hash one node's payload (data + extra slice). kind + main_token are
// hashed by the caller. Mirrors ast_visit_children's per-kind switch
// case-for-case. NODE = a child AstNodeId (relativized to node_min);
// TOK = a token index (relativized to tok_min); RAW = StrId / literal /
// count / meta (position-stable, hashed verbatim). The correctness rule
// is COVERAGE: every slot of every kind is hashed. A wrong NODE/RAW tag
// only causes a false recompute (perf); an OMITTED slot could collide
// two distinct decls (stale result) — so this must stay in lockstep
// with ast_visit_children above.
static uint64_t ast_hash_node(const ASTStore *ast, uint32_t id,
                              uint32_t node_min, uint32_t tok_min, uint64_t h) {
  AstNodeKind k = ((const AstNodeKind *)ast->kinds.data)[id];
  AstNodeData d = ((const AstNodeData *)ast->data.data)[id];
  const uint32_t *ex = &((const uint32_t *)ast->extra.data)[d.extra_idx.idx];
#define NODE(v) (h = fp_u32(h, fp_rel_node((v), node_min)))
#define TOK(v) (h = fp_u32(h, (uint32_t)((v) - tok_min)))
#define RAW(v) (h = fp_u32(h, (uint32_t)(v)))

  switch (k) {
  // Leaves — data is a literal / StrId; hash the full 8-byte payload.
  case AST_ERROR:
  case AST_EXPR_PATH:
  case AST_EXPR_LIT_INT:
  case AST_EXPR_LIT_FLOAT:
  case AST_EXPR_LIT_STRING:
  case AST_EXPR_LIT_BYTE:
  case AST_EXPR_LIT_BOOL:
  case AST_EXPR_LIT_NIL:
  case AST_EXPR_ASM:
  case AST_EXPR_WILDCARD:
  case AST_EXPR_ENUM_REF:
  case AST_STMT_BREAK:
  case AST_STMT_CONTINUE:
    return fp_u64(h, d.int_val);

  // single_child.
  case AST_STMT_RETURN:
  case AST_STMT_DEFER:
  case AST_TYPE_PTR:
  case AST_TYPE_SLICE:
  case AST_TYPE_MANYPTR:
  case AST_TYPE_OPTIONAL:
  case AST_TYPE_CONST:
  case AST_EXPR_UNARY_NEG:
  case AST_EXPR_UNARY_NOT:
  case AST_EXPR_UNARY_BIT_NOT:
  case AST_EXPR_UNARY_REF:
  case AST_EXPR_UNARY_DEREF:
  case AST_EXPR_UNARY_INC:
  case AST_EXPR_UNARY_DENIL:
  case AST_EXPR_UNARY_DEERR:
    NODE(d.single_child.idx);
    return h;

  // bin — lhs + rhs.
  case AST_EXPR_BIN_ADD:
  case AST_EXPR_BIN_SUB:
  case AST_EXPR_BIN_MUL:
  case AST_EXPR_BIN_DIV:
  case AST_EXPR_BIN_MOD:
  case AST_EXPR_BIN_POW:
  case AST_EXPR_BIN_EQ:
  case AST_EXPR_BIN_NEQ:
  case AST_EXPR_BIN_LT:
  case AST_EXPR_BIN_LE:
  case AST_EXPR_BIN_GT:
  case AST_EXPR_BIN_GE:
  case AST_EXPR_BIN_AND:
  case AST_EXPR_BIN_OR:
  case AST_EXPR_BIN_ORELSE:
  case AST_EXPR_BIN_CATCH:
  case AST_EXPR_BIN_BIT_AND:
  case AST_EXPR_BIN_BIT_OR:
  case AST_EXPR_BIN_BIT_XOR:
  case AST_EXPR_BIN_SHL:
  case AST_EXPR_BIN_SHR:
  case AST_EXPR_ASSIGN:
  case AST_EXPR_ASSIGN_ADD:
  case AST_EXPR_ASSIGN_SUB:
  case AST_EXPR_ASSIGN_MUL:
  case AST_EXPR_ASSIGN_DIV:
  case AST_EXPR_ASSIGN_MOD:
  case AST_EXPR_ASSIGN_BIT_AND:
  case AST_EXPR_ASSIGN_BIT_OR:
  case AST_EXPR_ASSIGN_BIT_XOR:
  case AST_EXPR_FIELD:
  case AST_EXPR_INDEX:
  case AST_EXPR_HANDLE:
  case AST_EXPR_MASK:
    NODE(d.bin.lhs.idx);
    NODE(d.bin.rhs.idx);
    return h;

  // extras-based kinds — layouts mirror ast_visit_children's comments.
  case AST_TYPE_ARRAY: // [size, elem]
    NODE(ex[0]);
    NODE(ex[1]);
    return h;

  case AST_EXPR_PRODUCT: { // [type, fcount, f0...]
    NODE(ex[0]);
    RAW(ex[1]);
    for (uint32_t i = 0; i < ex[1]; i++)
      NODE(ex[2 + i]);
    return h;
  }

  case AST_DECL_CONST: // [name_strid, type, value, meta]
  case AST_DECL_VAR:
    RAW(ex[0]);
    NODE(ex[1]);
    NODE(ex[2]);
    RAW(ex[3]);
    return h;

  case AST_DECL_DESTRUCTURE: // [pattern, type, value, meta]
    NODE(ex[0]);
    NODE(ex[1]);
    NODE(ex[2]);
    RAW(ex[3]);
    return h;

  case AST_EXPR_LAMBDA: { // [ret, body, effect, pcount, p0...]
    NODE(ex[0]);
    NODE(ex[1]);
    NODE(ex[2]);
    RAW(ex[3]);
    for (uint32_t i = 0; i < ex[3]; i++)
      NODE(ex[4 + i]);
    return h;
  }

  case AST_TYPE_FN: { // [ret, effect, pcount, p0...]
    NODE(ex[0]);
    NODE(ex[1]);
    RAW(ex[2]);
    for (uint32_t i = 0; i < ex[2]; i++)
      NODE(ex[3 + i]);
    return h;
  }

  case AST_STMT_BLOCK: { // [stmt_count, s0...]
    RAW(ex[0]);
    for (uint32_t i = 0; i < ex[0]; i++)
      NODE(ex[1 + i]);
    return h;
  }

  case AST_STMT_IF: // [cond, then, else]
    NODE(ex[0]);
    NODE(ex[1]);
    NODE(ex[2]);
    return h;

  case AST_STMT_SWITCH: { // [scrutinee, arm_count, arm0...]
    NODE(ex[0]);
    RAW(ex[1]);
    for (uint32_t i = 0; i < ex[1]; i++)
      NODE(ex[2 + i]);
    return h;
  }

  case AST_STMT_SWITCH_ARM: { // [pat_count, pat0..N, body]
    RAW(ex[0]);
    for (uint32_t i = 0; i < ex[0]; i++)
      NODE(ex[1 + i]);
    NODE(ex[1 + ex[0]]);
    return h;
  }

  case AST_EXPR_BUILTIN: { // [name_strid, arg_count, arg0...]
    RAW(ex[0]);
    RAW(ex[1]);
    for (uint32_t i = 0; i < ex[1]; i++)
      NODE(ex[2 + i]);
    return h;
  }

  case AST_STMT_LOOP: // [label_strid, init, cond, step, body]
    RAW(ex[0]);
    NODE(ex[1]);
    NODE(ex[2]);
    NODE(ex[3]);
    NODE(ex[4]);
    return h;

  case AST_DECL_MODULE: // [count, c0...]
  case AST_DECL_STRUCT:
  case AST_DECL_UNION:
  case AST_DECL_ENUM: {
    RAW(ex[0]);
    for (uint32_t i = 0; i < ex[0]; i++)
      NODE(ex[1 + i]);
    return h;
  }

  case AST_DECL_PARAM: // [name_strid, type, is_comptime]
    RAW(ex[0]);
    NODE(ex[1]);
    RAW(ex[2]);
    return h;

  case AST_DECL_FIELD: // [name_strid, type, vis, fpos]  (fpos IS a node)
    RAW(ex[0]);
    NODE(ex[1]);
    RAW(ex[2]);
    NODE(ex[3]);
    return h;

  case AST_DECL_VARIANT: // [name_strid, value]
  case AST_INIT_FIELD:   // [name_strid (0=positional), value]
    RAW(ex[0]);
    NODE(ex[1]);
    return h;

  case AST_EXPR_CALL: { // [callee, arg_count, arg0...]
    NODE(ex[0]);
    RAW(ex[1]);
    for (uint32_t i = 0; i < ex[1]; i++)
      NODE(ex[2 + i]);
    return h;
  }

  case AST_EXPR_SLICE: // [recv, lo, hi]
    NODE(ex[0]);
    NODE(ex[1]);
    NODE(ex[2]);
    return h;

  case AST_EXPR_HANDLER: { // [hdr, effect, initially, return, finally,
                           //  branch_count, (sort, name_tok, lambda)xN]
    RAW(ex[0]);            // hdr — header token/strid, not a child node
    NODE(ex[1]);
    NODE(ex[2]);
    NODE(ex[3]);
    NODE(ex[4]);
    RAW(ex[5]);
    for (uint32_t i = 0; i < ex[5]; i++) {
      RAW(ex[6 + i * 3]);     // sort
      TOK(ex[6 + i * 3 + 1]); // name_tok
      NODE(ex[6 + i * 3 + 2]);
    }
    return h;
  }

  case AST_EXPR_EFFECT_ROW: { // [flags, tail_strid, label_count, l0...]
    RAW(ex[0]);
    RAW(ex[1]);
    RAW(ex[2]);
    for (uint32_t i = 0; i < ex[2]; i++)
      NODE(ex[3 + i]);
    return h;
  }

  case AST_DECL_EFFECT: { // [hdr, in_type, tparam_count, tp..,
                          //  sig_count, (sort, name_tok, sig)xN]
    RAW(ex[0]);
    NODE(ex[1]);
    RAW(ex[2]);
    for (uint32_t i = 0; i < ex[2]; i++)
      NODE(ex[3 + i]);
    uint32_t sc_at = 3 + ex[2];
    RAW(ex[sc_at]);
    for (uint32_t i = 0; i < ex[sc_at]; i++) {
      RAW(ex[sc_at + 1 + i * 3]);     // sort
      TOK(ex[sc_at + 1 + i * 3 + 1]); // name_tok
      NODE(ex[sc_at + 1 + i * 3 + 2]);
    }
    return h;
  }
  }

  // Defensive: a kind not enumerated above still contributes its raw
  // 8-byte data — may over-hash (false recompute) but never collides.
  return fp_u64(h, d.int_val);
#undef NODE
#undef TOK
#undef RAW
}

uint64_t ast_subtree_fingerprint(const ASTStore *ast, AstNodeId root) {
  if (root.idx == 0 || root.idx >= ast->kinds.count)
    return 0;

  // Pass 1 — subtree extent: min node id, min main_token, node count.
  struct ast_extent e = {
      .ast = ast,
      .node_min = root.idx,
      .tok_min = ((const uint32_t *)ast->main_tokens.data)[root.idx],
      .count = 1,
  };
  ast_visit_children(ast, root, ast_extent_cb, &e);

  // [node_min, root] is a sub-range of the decl's exclusively-owned
  // node-id span — decls are parsed one at a time, post-order, so the
  // decl node is the highest id and node_min the lowest reachable one.
  // The range MAY also contain desugar-orphaned (unreachable) nodes —
  // e.g. the pre-rebuild call node a trailing-lambda leaves behind.
  // Those are deterministic from the decl's own text and shift
  // uniformly with it, so hashing them over the linear range is stable
  // and harmless. The one hard invariant: no reachable node lies
  // outside [node_min, root].
  assert(e.count <= root.idx - e.node_min + 1u &&
         "ast_subtree_fingerprint: reachable node outside the decl range");

  // Pass 2 — hash the slice [node_min, root] in ascending id order.
  uint64_t h = AST_FP_BASIS;
  h = fp_u32(h, e.count);
  const AstNodeKind *kinds = (const AstNodeKind *)ast->kinds.data;
  const uint32_t *toks = (const uint32_t *)ast->main_tokens.data;
  for (uint32_t id = e.node_min; id <= root.idx; id++) {
    h = fp_u32(h, (uint32_t)kinds[id]);
    h = fp_u32(h, toks[id] - e.tok_min);
    h = ast_hash_node(ast, id, e.node_min, e.tok_min, h);
  }
  return h;
}

void ast_store_free(ASTStore *ast) {
  if (!ast)
    return;
  vec_free(&ast->kinds);
  vec_free(&ast->main_tokens);
  vec_free(&ast->data);
  vec_free(&ast->extra);
}

// Single source of truth for AstNodeKind → string. Used by sema's
// "not yet implemented" diagnostics and by ast_dump_inc.h's dumper.
const char *ast_kind_name(AstNodeKind kind) {
  switch (kind) {
  case AST_ERROR:
    return "AST_ERROR";
  case AST_DECL_MODULE:
    return "AST_DECL_MODULE";
  case AST_DECL_STRUCT:
    return "AST_DECL_STRUCT";
  case AST_DECL_ENUM:
    return "AST_DECL_ENUM";
  case AST_DECL_UNION:
    return "AST_DECL_UNION";
  case AST_DECL_EFFECT:
    return "AST_DECL_EFFECT";
  case AST_DECL_CONST:
    return "AST_DECL_CONST";
  case AST_DECL_VAR:
    return "AST_DECL_VAR";
  case AST_DECL_DESTRUCTURE:
    return "AST_DECL_DESTRUCTURE";
  case AST_DECL_PARAM:
    return "AST_DECL_PARAM";
  case AST_DECL_FIELD:
    return "AST_DECL_FIELD";
  case AST_DECL_VARIANT:
    return "AST_DECL_VARIANT";
  case AST_STMT_BLOCK:
    return "AST_STMT_BLOCK";
  case AST_STMT_RETURN:
    return "AST_STMT_RETURN";
  case AST_STMT_IF:
    return "AST_STMT_IF";
  case AST_STMT_LOOP:
    return "AST_STMT_LOOP";
  case AST_STMT_SWITCH:
    return "AST_STMT_SWITCH";
  case AST_STMT_SWITCH_ARM:
    return "AST_STMT_SWITCH_ARM";
  case AST_STMT_BREAK:
    return "AST_STMT_BREAK";
  case AST_STMT_CONTINUE:
    return "AST_STMT_CONTINUE";
  case AST_STMT_DEFER:
    return "AST_STMT_DEFER";
  case AST_EXPR_LIT_INT:
    return "AST_EXPR_LIT_INT";
  case AST_EXPR_LIT_FLOAT:
    return "AST_EXPR_LIT_FLOAT";
  case AST_EXPR_LIT_STRING:
    return "AST_EXPR_LIT_STRING";
  case AST_EXPR_LIT_BYTE:
    return "AST_EXPR_LIT_BYTE";
  case AST_EXPR_LIT_BOOL:
    return "AST_EXPR_LIT_BOOL";
  case AST_EXPR_LIT_NIL:
    return "AST_EXPR_LIT_NIL";
  case AST_EXPR_ASM:
    return "AST_EXPR_ASM";
  case AST_EXPR_WILDCARD:
    return "AST_EXPR_WILDCARD";
  case AST_EXPR_BIN_ADD:
    return "AST_EXPR_BIN_ADD";
  case AST_EXPR_BIN_SUB:
    return "AST_EXPR_BIN_SUB";
  case AST_EXPR_BIN_MUL:
    return "AST_EXPR_BIN_MUL";
  case AST_EXPR_BIN_DIV:
    return "AST_EXPR_BIN_DIV";
  case AST_EXPR_BIN_MOD:
    return "AST_EXPR_BIN_MOD";
  case AST_EXPR_BIN_POW:
    return "AST_EXPR_BIN_POW";
  case AST_EXPR_BIN_EQ:
    return "AST_EXPR_BIN_EQ";
  case AST_EXPR_BIN_NEQ:
    return "AST_EXPR_BIN_NEQ";
  case AST_EXPR_BIN_LT:
    return "AST_EXPR_BIN_LT";
  case AST_EXPR_BIN_LE:
    return "AST_EXPR_BIN_LE";
  case AST_EXPR_BIN_GT:
    return "AST_EXPR_BIN_GT";
  case AST_EXPR_BIN_GE:
    return "AST_EXPR_BIN_GE";
  case AST_EXPR_BIN_AND:
    return "AST_EXPR_BIN_AND";
  case AST_EXPR_BIN_OR:
    return "AST_EXPR_BIN_OR";
  case AST_EXPR_BIN_ORELSE:
    return "AST_EXPR_BIN_ORELSE";
  case AST_EXPR_BIN_CATCH:
    return "AST_EXPR_BIN_CATCH";
  case AST_EXPR_BIN_BIT_AND:
    return "AST_EXPR_BIN_BIT_AND";
  case AST_EXPR_BIN_BIT_OR:
    return "AST_EXPR_BIN_BIT_OR";
  case AST_EXPR_BIN_BIT_XOR:
    return "AST_EXPR_BIN_BIT_XOR";
  case AST_EXPR_BIN_SHL:
    return "AST_EXPR_BIN_SHL";
  case AST_EXPR_BIN_SHR:
    return "AST_EXPR_BIN_SHR";
  case AST_EXPR_ASSIGN:
    return "AST_EXPR_ASSIGN";
  case AST_EXPR_ASSIGN_ADD:
    return "AST_EXPR_ASSIGN_ADD";
  case AST_EXPR_ASSIGN_SUB:
    return "AST_EXPR_ASSIGN_SUB";
  case AST_EXPR_ASSIGN_MUL:
    return "AST_EXPR_ASSIGN_MUL";
  case AST_EXPR_ASSIGN_DIV:
    return "AST_EXPR_ASSIGN_DIV";
  case AST_EXPR_ASSIGN_MOD:
    return "AST_EXPR_ASSIGN_MOD";
  case AST_EXPR_ASSIGN_BIT_AND:
    return "AST_EXPR_ASSIGN_BIT_AND";
  case AST_EXPR_ASSIGN_BIT_OR:
    return "AST_EXPR_ASSIGN_BIT_OR";
  case AST_EXPR_ASSIGN_BIT_XOR:
    return "AST_EXPR_ASSIGN_BIT_XOR";
  case AST_EXPR_UNARY_NEG:
    return "AST_EXPR_UNARY_NEG";
  case AST_EXPR_UNARY_NOT:
    return "AST_EXPR_UNARY_NOT";
  case AST_EXPR_UNARY_BIT_NOT:
    return "AST_EXPR_UNARY_BIT_NOT";
  case AST_EXPR_UNARY_REF:
    return "AST_EXPR_UNARY_REF";
  case AST_EXPR_UNARY_DEREF:
    return "AST_EXPR_UNARY_DEREF";
  case AST_EXPR_UNARY_INC:
    return "AST_EXPR_UNARY_INC";
  case AST_EXPR_UNARY_DENIL:
    return "AST_EXPR_UNARY_DENIL";
  case AST_EXPR_UNARY_DEERR:
    return "AST_EXPR_UNARY_DEERR";
  case AST_EXPR_CALL:
    return "AST_EXPR_CALL";
  case AST_EXPR_INDEX:
    return "AST_EXPR_INDEX";
  case AST_EXPR_SLICE:
    return "AST_EXPR_SLICE";
  case AST_EXPR_FIELD:
    return "AST_EXPR_FIELD";
  case AST_EXPR_PATH:
    return "AST_EXPR_PATH";
  case AST_EXPR_LAMBDA:
    return "AST_EXPR_LAMBDA";
  case AST_EXPR_HANDLE:
    return "AST_EXPR_HANDLE";
  case AST_EXPR_HANDLER:
    return "AST_EXPR_HANDLER";
  case AST_EXPR_MASK:
    return "AST_EXPR_MASK";
  case AST_EXPR_PRODUCT:
    return "AST_EXPR_PRODUCT";
  case AST_INIT_FIELD:
    return "AST_INIT_FIELD";
  case AST_EXPR_ENUM_REF:
    return "AST_EXPR_ENUM_REF";
  case AST_EXPR_BUILTIN:
    return "AST_EXPR_BUILTIN";
  case AST_EXPR_EFFECT_ROW:
    return "AST_EXPR_EFFECT_ROW";
  case AST_TYPE_PTR:
    return "AST_TYPE_PTR";
  case AST_TYPE_SLICE:
    return "AST_TYPE_SLICE";
  case AST_TYPE_ARRAY:
    return "AST_TYPE_ARRAY";
  case AST_TYPE_MANYPTR:
    return "AST_TYPE_MANYPTR";
  case AST_TYPE_FN:
    return "AST_TYPE_FN";
  case AST_TYPE_OPTIONAL:
    return "AST_TYPE_OPTIONAL";
  case AST_TYPE_CONST:
    return "AST_TYPE_CONST";
  default:
    return "UNKNOWN";
  }
}