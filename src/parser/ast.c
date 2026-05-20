#include "ast.h"

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
  case AST_TYPE_VOID:
  case AST_TYPE_NORETURN:
  case AST_TYPE_ANYTYPE:
  case AST_TYPE_TYPE:
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

  case AST_DECL_PARAM: // [name, type, is_comptime]
    EMIT(ex[0]);
    EMIT(ex[1]);
    return;

  case AST_DECL_FIELD: // [name, type, vis, fpos]
    EMIT(ex[0]);
    EMIT(ex[1]);
    EMIT(ex[3]);
    return;

  case AST_DECL_VARIANT: // [name, value]
  case AST_INIT_FIELD:
    EMIT(ex[0]);
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

void ast_store_free(ASTStore *ast) {
  if (!ast)
    return;
  vec_free(&ast->kinds);
  vec_free(&ast->main_tokens);
  vec_free(&ast->data);
  vec_free(&ast->extra);
}