#ifndef AST_DUMP_INC_H
#define AST_DUMP_INC_H

#include "../../parser/ast.h"
#include "../../db/storage/stringpool.h"
#include "../../db/db.h"
#include <stdio.h>

static const char* ast_kind_name(AstNodeKind kind) {
    switch (kind) {
        case AST_DECL_MODULE: return "AST_DECL_MODULE";
        case AST_DECL_IMPORT: return "AST_DECL_IMPORT";
        case AST_DECL_FN: return "AST_DECL_FN";
        case AST_DECL_STRUCT: return "AST_DECL_STRUCT";
        case AST_DECL_ENUM: return "AST_DECL_ENUM";
        case AST_DECL_UNION: return "AST_DECL_UNION";
        case AST_DECL_EFFECT: return "AST_DECL_EFFECT";
        case AST_DECL_HANDLER: return "AST_DECL_HANDLER";
        case AST_DECL_DISTINCT: return "AST_DECL_DISTINCT";
        case AST_DECL_TYPE: return "AST_DECL_TYPE";
        case AST_DECL_CONST: return "AST_DECL_CONST";
        case AST_DECL_VAL: return "AST_DECL_VAL";
        case AST_STMT_BLOCK: return "AST_STMT_BLOCK";
        case AST_STMT_EXPR: return "AST_STMT_EXPR";
        case AST_STMT_RETURN: return "AST_STMT_RETURN";
        case AST_STMT_IF: return "AST_STMT_IF";
        case AST_STMT_LOOP: return "AST_STMT_LOOP";
        case AST_STMT_SWITCH: return "AST_STMT_SWITCH";
        case AST_STMT_BREAK: return "AST_STMT_BREAK";
        case AST_STMT_CONTINUE: return "AST_STMT_CONTINUE";
        case AST_STMT_DEFER: return "AST_STMT_DEFER";
        case AST_EXPR_LIT_INT: return "AST_EXPR_LIT_INT";
        case AST_EXPR_LIT_FLOAT: return "AST_EXPR_LIT_FLOAT";
        case AST_EXPR_LIT_STRING: return "AST_EXPR_LIT_STRING";
        case AST_EXPR_LIT_BYTE: return "AST_EXPR_LIT_BYTE";
        case AST_EXPR_LIT_BOOL: return "AST_EXPR_LIT_BOOL";
        case AST_EXPR_LIT_NIL: return "AST_EXPR_LIT_NIL";
        case AST_EXPR_ASM: return "AST_EXPR_ASM";
        case AST_EXPR_WILDCARD: return "AST_EXPR_WILDCARD";
        case AST_EXPR_BIN_ADD: return "AST_EXPR_BIN_ADD";
        case AST_EXPR_BIN_SUB: return "AST_EXPR_BIN_SUB";
        case AST_EXPR_BIN_MUL: return "AST_EXPR_BIN_MUL";
        case AST_EXPR_BIN_DIV: return "AST_EXPR_BIN_DIV";
        case AST_EXPR_BIN_MOD: return "AST_EXPR_BIN_MOD";
        case AST_EXPR_BIN_EQ: return "AST_EXPR_BIN_EQ";
        case AST_EXPR_BIN_NEQ: return "AST_EXPR_BIN_NEQ";
        case AST_EXPR_BIN_LT: return "AST_EXPR_BIN_LT";
        case AST_EXPR_BIN_LE: return "AST_EXPR_BIN_LE";
        case AST_EXPR_BIN_GT: return "AST_EXPR_BIN_GT";
        case AST_EXPR_BIN_GE: return "AST_EXPR_BIN_GE";
        case AST_EXPR_BIN_AND: return "AST_EXPR_BIN_AND";
        case AST_EXPR_BIN_OR: return "AST_EXPR_BIN_OR";
        case AST_EXPR_BIN_BIT_AND: return "AST_EXPR_BIN_BIT_AND";
        case AST_EXPR_BIN_BIT_OR: return "AST_EXPR_BIN_BIT_OR";
        case AST_EXPR_BIN_BIT_XOR: return "AST_EXPR_BIN_BIT_XOR";
        case AST_EXPR_BIN_SHL: return "AST_EXPR_BIN_SHL";
        case AST_EXPR_BIN_SHR: return "AST_EXPR_BIN_SHR";
        case AST_EXPR_BIN_POW: return "AST_EXPR_BIN_POW";
        case AST_EXPR_BIN_ORELSE: return "AST_EXPR_BIN_ORELSE";
        case AST_EXPR_BIN_CATCH: return "AST_EXPR_BIN_CATCH";
        case AST_EXPR_ASSIGN: return "AST_EXPR_ASSIGN";
        case AST_EXPR_ASSIGN_ADD: return "AST_EXPR_ASSIGN_ADD";
        case AST_EXPR_ASSIGN_SUB: return "AST_EXPR_ASSIGN_SUB";
        case AST_EXPR_ASSIGN_MUL: return "AST_EXPR_ASSIGN_MUL";
        case AST_EXPR_ASSIGN_DIV: return "AST_EXPR_ASSIGN_DIV";
        case AST_EXPR_ASSIGN_MOD: return "AST_EXPR_ASSIGN_MOD";
        case AST_EXPR_ASSIGN_BIT_AND: return "AST_EXPR_ASSIGN_BIT_AND";
        case AST_EXPR_ASSIGN_BIT_OR: return "AST_EXPR_ASSIGN_BIT_OR";
        case AST_EXPR_ASSIGN_BIT_XOR: return "AST_EXPR_ASSIGN_BIT_XOR";
        case AST_EXPR_UNARY_NEG: return "AST_EXPR_UNARY_NEG";
        case AST_EXPR_UNARY_NOT: return "AST_EXPR_UNARY_NOT";
        case AST_EXPR_UNARY_BIT_NOT: return "AST_EXPR_UNARY_BIT_NOT";
        case AST_EXPR_UNARY_REF: return "AST_EXPR_UNARY_REF";
        case AST_EXPR_UNARY_DEREF: return "AST_EXPR_UNARY_DEREF";
        case AST_EXPR_UNARY_PTR: return "AST_EXPR_UNARY_PTR";
        case AST_EXPR_UNARY_OPTIONAL: return "AST_EXPR_UNARY_OPTIONAL";
        case AST_EXPR_UNARY_CONST: return "AST_EXPR_UNARY_CONST";
        case AST_EXPR_UNARY_INC: return "AST_EXPR_UNARY_INC";
        case AST_EXPR_UNARY_DENIL: return "AST_EXPR_UNARY_DENIL";
        case AST_EXPR_UNARY_DEERR: return "AST_EXPR_UNARY_DEERR";
        case AST_EXPR_CALL: return "AST_EXPR_CALL";
        case AST_EXPR_INDEX: return "AST_EXPR_INDEX";
        case AST_EXPR_SLICE: return "AST_EXPR_SLICE";
        case AST_EXPR_FIELD: return "AST_EXPR_FIELD";
        case AST_EXPR_PATH: return "AST_EXPR_PATH";
        case AST_EXPR_GROUP: return "AST_EXPR_GROUP";
        case AST_EXPR_LAMBDA: return "AST_EXPR_LAMBDA";
        case AST_EXPR_HANDLE: return "AST_EXPR_HANDLE";
        case AST_EXPR_HANDLER: return "AST_EXPR_HANDLER";
        case AST_EXPR_MASK: return "AST_EXPR_MASK";
        case AST_EXPR_WITH: return "AST_EXPR_WITH";
        case AST_EXPR_PRODUCT: return "AST_EXPR_PRODUCT";
        case AST_EXPR_ENUM_REF: return "AST_EXPR_ENUM_REF";
        case AST_EXPR_ARRAY_LIT: return "AST_EXPR_ARRAY_LIT";
        case AST_EXPR_BUILTIN: return "AST_EXPR_BUILTIN";
        case AST_EXPR_EFFECT_ROW: return "AST_EXPR_EFFECT_ROW";
        case AST_TYPE_PATH: return "AST_TYPE_PATH";
        case AST_TYPE_PTR: return "AST_TYPE_PTR";
        case AST_TYPE_SLICE: return "AST_TYPE_SLICE";
        case AST_TYPE_ARRAY: return "AST_TYPE_ARRAY";
        case AST_TYPE_MANYPTR: return "AST_TYPE_MANYPTR";
        case AST_TYPE_FN: return "AST_TYPE_FN";
        case AST_TYPE_OPTIONAL: return "AST_TYPE_OPTIONAL";
        case AST_TYPE_CONST: return "AST_TYPE_CONST";
        default: return "UNKNOWN";
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
        printf(" '%s'\n", pool_get(strings, data.string_id));
        return;
    }
    
    if (kind == AST_EXPR_LIT_INT || kind == AST_EXPR_LIT_FLOAT || kind == AST_EXPR_LIT_STRING) {
        printf(" '%s'\n", pool_get(strings, data.string_id));
        return;
    }
    
    printf("\n");
    
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
        print_indent(indent + 1); printf("Name:\n");
        dump_ast_node(ast, name_id, indent + 2, strings);
        for (uint32_t i=0; i<param_count; i++) {
            print_indent(indent + 1); printf("Param %u:\n", i);
            dump_ast_node(ast, (AstNodeId){extra[4+i]}, indent + 2, strings);
        }
        if (ret_type.idx) {
            print_indent(indent + 1); printf("Returns:\n");
            dump_ast_node(ast, ret_type, indent + 2, strings);
        }
        if (body.idx) {
            print_indent(indent + 1); printf("Body:\n");
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

void ast_dump_module(ASTStore *ast, Vec *top_level_index, StringPool *strings) {
    if (top_level_index && top_level_index->count > 0) {
        printf("Top-Level Index:\n");
        for (size_t i = 0; i < top_level_index->count; i++) {
            TopLevelEntry *e = (TopLevelEntry*)vec_get(top_level_index, i);
            printf("  - %s (Node: %u, Vis: %s, AstId: %08x)\n", 
                pool_get(strings, e->name), e->node.idx, 
                e->vis == VIS_PUBLIC ? "pub" : "private",
                e->ast_id.idx);
        }
        printf("\n");
    }

    if (ast->kinds.count <= 1) return;
    printf("AST Structure:\n");
    AstNodeId root = { .idx = (uint32_t)(ast->kinds.count - 1) };
    dump_ast_node(ast, root, 0, strings);
}

#endif // AST_DUMP_INC_H
