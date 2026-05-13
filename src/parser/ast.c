#include "ast.h"
#include <stdint.h>
#include <string.h>


void ast_store_int(ASTStore* ast, Arena* arena) {
    ast->arena = arena;

    // Init vectors
    vec_init_in(&ast->node_kinds, arena, sizeof(AstNodeKind));
    vec_init_in(&ast->node_data, arena, sizeof(AstNodeData));
    vec_init_in(&ast->extra_data, arena, sizeof(uint32_t));
    vec_init_in(&ast->span_map, arena, sizeof(struct Span));
    vec_init_in(&ast->parent_map, arena, sizeof(AstNodeId));

    // vec_init_in(ast->trivia_map, arena, sizeof(struct Token));
    // vec_init_in(ast->type_map, arena, sizeof(struct Token));

    // Push the Sentinel Node (Index 0)
    AstNodeData dummy_data = {0};
    struct Span dummy_span = {0};

    vec_push(&ast->node_kinds, &AST_NODE_NONE);
    vec_push(&ast->node_data, &dummy_data);
    vec_push(&ast->span_map, &dummy_span);
}

AstNodeId ast_push_node(ASTStore* ast, AstNodeKind kind, AstNodeData data, struct Span span) {
    // the length of the vector before pushing new ID.
    uint32_t id = ast->node_kinds.count;

    vec_push(&ast->node_kinds, &kind);
    vec_push(&ast->node_data, &data);
    vec_push(&ast->span_map, &span);

    return (AstNodeId){ .idx = id };
}

void build_parent_map(ASTStore* ast) {
    uint32_t node_count = ast->node_kinds.count;

    vec_resize_zeroed(&ast->parent_map, ast->arena, node_count);

    AstNodeKind* kinds = (AstNodeKind*)ast->node_kinds.data;
    AstNodeData* data = (AstNodeData*)ast->node_data.data;
    AstNodeId* parents = (AstNodeId*)ast->parent_map.data;
    uint32_t* extra = (uint32_t*)ast->extra_data.data;

    // Linear scan.
    for (uint32_t i = 1; i < node_count; i++) {
        AstNodeId current = { .idx = i };
        AstNodeKind k = kinds[i];
        AstNodeData d = data[i];

        // Binary Exprs
        if (k >= AST_EXPR_BIN_ADD && k <= AST_EXPR_BIN_ASSIGN) {
            parents[d.pair.lhs.idx] = current;
            parents[d.pair.rhs.idx] = current;
        }
        // Unary / Grouping
        else if ((k >= AST_EXPR_UNARY_NEGATE && k <= AST_EXPR_UNARY_NOT) || 
                 k == AST_EXPR_GROUPING || k == AST_STMT_EXPR || k == AST_STMT_RETURN) {
            if (ast_id_valid(d.single_child)) {
                parents[d.single_child.idx] = current;
            }
        }
        // Large Nodes (Look into extra_data)
        else if (k == AST_DECL_FUNC) {
            uint32_t idx = d.extra_data_idx.raw;
            // Assuming FuncDecl packs: [name_id, type_id, params_id, body_id]
            parents[extra[idx]] = current;     // Name
            parents[extra[idx + 1]] = current; // Type
            parents[extra[idx + 2]] = current; // Params List
            parents[extra[idx + 3]] = current; // Body
        }
        // Literals have no children, do nothing.
    }
}