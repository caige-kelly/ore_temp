#include "ast.h"

void ast_store_init(ASTStore *ast, Arena *arena, size_t max_nodes) {
  (void)arena;
  (void)max_nodes;
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

void ast_store_free(ASTStore *ast) {
  if (!ast)
    return;
  vec_free(&ast->kinds);
  vec_free(&ast->main_tokens);
  vec_free(&ast->data);
  vec_free(&ast->extra);
}