#include "ast.h"

void ast_store_init(ASTStore *ast, Arena *arena, size_t max_nodes) {
    ast->arena = arena;
    
    // Allocate fixed-capacity vectors in the arena.
    // The parser knows an upper bound (the number of tokens).
    vec_init_in_arena(&ast->kinds, arena, max_nodes, sizeof(AstNodeKind));
    vec_init_in_arena(&ast->main_tokens, arena, max_nodes, sizeof(uint32_t));
    vec_init_in_arena(&ast->data, arena, max_nodes, sizeof(AstNodeData));
    vec_init_in_arena(&ast->extra, arena, max_nodes * 2, sizeof(uint32_t));
    
    // Push sentinel node 0
    AstNodeKind dummy_kind = AST_ERROR;
    uint32_t dummy_token = 0;
    AstNodeData dummy_data = {0};
    
    vec_push(&ast->kinds, &dummy_kind);
    vec_push(&ast->main_tokens, &dummy_token);
    vec_push(&ast->data, &dummy_data);
}

AstNodeId ast_push_node(ASTStore *ast, AstNodeKind kind, uint32_t main_token, AstNodeData data) {
    uint32_t idx = ast->kinds.count;
    
    vec_push(&ast->kinds, &kind);
    vec_push(&ast->main_tokens, &main_token);
    vec_push(&ast->data, &data);
    
    return (AstNodeId){ .idx = idx };
}

AstExtraDataIdx ast_push_extra(ASTStore *ast, const uint32_t *items, uint32_t count) {
    uint32_t idx = ast->extra.count;
    for (uint32_t i = 0; i < count; i++) {
        vec_push(&ast->extra, &items[i]);
    }
    return (AstExtraDataIdx){ .idx = idx };
}