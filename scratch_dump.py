import re

with open('src/parser/ast.h') as f:
    ast_h = f.read()

kinds = re.findall(r'AST_[A-Z_0-9]+', ast_h)

kind_strs = []
for k in kinds:
    if k not in kind_strs and k not in ['AST_H', 'AST_ERROR']:
        kind_strs.append(k)

c_code = """
#include "../../parser/ast.h"
#include "../../db/storage/stringpool.h"
#include <stdio.h>

static const char* ast_kind_name(AstNodeKind kind) {
    switch (kind) {
"""
for k in kind_strs:
    c_code += f'        case {k}: return "{k}";\n'
c_code += """        default: return "UNKNOWN";
    }
}

static void print_indent(int indent) {
    for (int i = 0; i < indent; i++) printf("  ");
}

static void dump_ast_node(ASTStore *ast, AstNodeId id, int indent, StringPool *strings) {
    if (id.idx == 0) return;
    AstNodeKind kind = ((AstNodeKind*)ast->kinds.data)[id.idx];
    AstNodeData data = ((AstNodeData*)ast->data.data)[id.idx];
    
    print_indent(indent);
    printf("%s", ast_kind_name(kind));
    
    if (kind == AST_EXPR_PATH || kind == AST_TYPE_PATH) {
        printf(" '%s'\\n", pool_get(strings, data.string_id));
        return;
    }
    
    if (kind == AST_EXPR_LIT_INT) {
        printf(" %llu\\n", (unsigned long long)data.int_val);
        return;
    }
    
    printf("\\n");
    
    if (kind == AST_STMT_EXPR || kind == AST_STMT_RETURN || kind == AST_STMT_DEFER || kind == AST_TYPE_PTR || kind == AST_TYPE_SLICE) {
        dump_ast_node(ast, data.single_child, indent + 1, strings);
        return;
    }
    
    if (kind >= AST_EXPR_BIN_ADD && kind <= AST_EXPR_BIN_SHR) {
        dump_ast_node(ast, data.bin.lhs, indent + 1, strings);
        dump_ast_node(ast, data.bin.rhs, indent + 1, strings);
        return;
    }
    
    if (kind >= AST_EXPR_ASSIGN && kind <= AST_EXPR_ASSIGN_BIT_XOR) {
        dump_ast_node(ast, data.bin.lhs, indent + 1, strings);
        dump_ast_node(ast, data.bin.rhs, indent + 1, strings);
        return;
    }
    
    if (kind == AST_DECL_FN) {
        uint32_t *extra = &((uint32_t*)ast->extra.data)[data.extra_idx.idx];
        AstNodeId name_id = {extra[0]};
        AstNodeId ret_type = {extra[1]};
        AstNodeId body = {extra[2]};
        uint32_t param_count = extra[3];
        print_indent(indent + 1); printf("Name:\\n");
        dump_ast_node(ast, name_id, indent + 2, strings);
        for (uint32_t i=0; i<param_count; i++) {
            print_indent(indent + 1); printf("Param %u:\\n", i);
            dump_ast_node(ast, (AstNodeId){extra[4+i]}, indent + 2, strings);
        }
        if (ret_type.idx) {
            print_indent(indent + 1); printf("Returns:\\n");
            dump_ast_node(ast, ret_type, indent + 2, strings);
        }
        if (body.idx) {
            print_indent(indent + 1); printf("Body:\\n");
            dump_ast_node(ast, body, indent + 2, strings);
        }
        return;
    }
    
    if (kind == AST_STMT_BLOCK) {
        uint32_t *extra = &((uint32_t*)ast->extra.data)[data.extra_idx.idx];
        uint32_t stmt_count = extra[0];
        for (uint32_t i=0; i<stmt_count; i++) {
            dump_ast_node(ast, (AstNodeId){extra[i+1]}, indent + 1, strings);
        }
        return;
    }
    
    if (kind == AST_DECL_MODULE) {
        uint32_t *extra = &((uint32_t*)ast->extra.data)[data.extra_idx.idx];
        uint32_t count = extra[0];
        for (uint32_t i=0; i<count; i++) {
            dump_ast_node(ast, (AstNodeId){extra[i+1]}, indent + 1, strings);
        }
        return;
    }
    
    if (kind == AST_DECL_VAL) {
        uint32_t *extra = &((uint32_t*)ast->extra.data)[data.extra_idx.idx];
        dump_ast_node(ast, (AstNodeId){extra[0]}, indent + 1, strings);
        dump_ast_node(ast, (AstNodeId){extra[1]}, indent + 1, strings);
        return;
    }
    
    if (kind == AST_EXPR_CALL) {
        uint32_t *extra = &((uint32_t*)ast->extra.data)[data.extra_idx.idx];
        dump_ast_node(ast, (AstNodeId){extra[0]}, indent + 1, strings);
        uint32_t argc = extra[1];
        for (uint32_t i=0; i<argc; i++) {
            dump_ast_node(ast, (AstNodeId){extra[2+i]}, indent + 2, strings);
        }
        return;
    }
}

void ast_dump_module(ASTStore *ast, StringPool *strings) {
    if (ast->kinds.count <= 1) return;
    AstNodeId root = { .idx = ast->kinds.count - 1 };
    dump_ast_node(ast, root, 0, strings);
}
"""

with open('src/consumers/driver/ast_dump_inc.c', 'w') as f:
    f.write(c_code)
