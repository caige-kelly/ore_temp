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
        case AST_DECL_TYPE: return "AST_DECL_TYPE";
        case AST_DECL_CONST: return "AST_DECL_CONST";
        case AST_DECL_VAR: return "AST_DECL_VAR";
        case AST_DECL_VAL: return "AST_DECL_VAL";
        case AST_DECL_DESTRUCTURE: return "AST_DECL_DESTRUCTURE";
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
        case AST_EXPR_UNARY_INC: return "AST_EXPR_UNARY_INC";
        case AST_EXPR_UNARY_DENIL: return "AST_EXPR_UNARY_DENIL";
        case AST_EXPR_UNARY_DEERR: return "AST_EXPR_UNARY_DEERR";
        case AST_EXPR_CALL: return "AST_EXPR_CALL";
        case AST_EXPR_INDEX: return "AST_EXPR_INDEX";
        case AST_EXPR_SLICE: return "AST_EXPR_SLICE";
        case AST_EXPR_FIELD: return "AST_EXPR_FIELD";
        case AST_EXPR_PATH: return "AST_EXPR_PATH";
        case AST_TYPE_VOID: return "AST_TYPE_VOID";
        case AST_TYPE_NORETURN: return "AST_TYPE_NORETURN";
        case AST_TYPE_ANYTYPE: return "AST_TYPE_ANYTYPE";
        case AST_TYPE_TYPE: return "AST_TYPE_TYPE";
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
    
    if (kind == AST_EXPR_PATH) {
        printf(" '%s'\n", pool_get(strings, data.string_id));
        return;
    }
    
    if (kind == AST_EXPR_LIT_INT || kind == AST_EXPR_LIT_FLOAT || kind == AST_EXPR_LIT_STRING) {
        printf(" '%s'\n", pool_get(strings, data.string_id));
        return;
    }
    
    printf("\n");
    
    // single_child group: statement-wrappers, type-position prefix
    // unaries, AND the full UNARY family. All AST_EXPR_UNARY_* use
    // data.single_child uniformly (parse_prefix_unary / postfix
    // handlers), so one range check covers prefix NEG/NOT/BIT_NOT/REF/
    // PTR/OPTIONAL/CONST AND postfix DEREF/INC/DENIL/DEERR — fixes the
    // previous omission where `[]const u8` / `^i32` / `?u8` printed
    // the unary node with no child.
    if (kind == AST_STMT_EXPR || kind == AST_STMT_RETURN ||
        kind == AST_STMT_DEFER || kind == AST_TYPE_PTR ||
        kind == AST_TYPE_SLICE || kind == AST_TYPE_MANYPTR ||
        kind == AST_TYPE_OPTIONAL || kind == AST_TYPE_CONST ||
        (kind >= AST_EXPR_UNARY_NEG && kind <= AST_EXPR_UNARY_DEERR)) {
        dump_ast_node(ast, data.single_child, indent + 1, strings);
        return;
    }

    // Array type `[N]T` / `[_]T`: extras = [size_id (0 for `[_]`), elem_id].
    if (kind == AST_TYPE_ARRAY) {
        uint32_t *extra = &((uint32_t*)ast->extra.data)[data.extra_idx.idx];
        AstNodeId size = { extra[0] };
        AstNodeId elem = { extra[1] };
        if (size.idx) {
            print_indent(indent + 1); printf("size:\n");
            dump_ast_node(ast, size, indent + 2, strings);
        }
        if (elem.idx) {
            print_indent(indent + 1); printf("elem:\n");
            dump_ast_node(ast, elem, indent + 2, strings);
        }
        return;
    }

    if (kind == AST_EXPR_FIELD) { // bin: lhs=receiver, rhs=name path
        dump_ast_node(ast, data.bin.lhs, indent + 1, strings);
        dump_ast_node(ast, data.bin.rhs, indent + 1, strings);
        return;
    }

    // Aggregate construction: extras = [type_id (0=anon `.{}`),
    // field_count, field0..]; each field AST_DECL_VAL [name, value].
    if (kind == AST_EXPR_PRODUCT) {
        uint32_t *extra = &((uint32_t*)ast->extra.data)[data.extra_idx.idx];
        AstNodeId type_id = { extra[0] };
        uint32_t fcount = extra[1];
        if (type_id.idx) {
            print_indent(indent + 1); printf("type:\n");
            dump_ast_node(ast, type_id, indent + 2, strings);
        }
        for (uint32_t i = 0; i < fcount; i++)
            dump_ast_node(ast, (AstNodeId){extra[2 + i]}, indent + 1, strings);
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
    
    // Unified bind: extras = [name_strid, type_id, value_id, meta].
    if (kind == AST_DECL_CONST || kind == AST_DECL_VAR) {
        uint32_t *extra = &((uint32_t*)ast->extra.data)[data.extra_idx.idx];
        StrId name = { extra[0] };
        AstNodeId type_id = { extra[1] };
        AstNodeId value_id = { extra[2] };
        DefMeta meta = (DefMeta)extra[3];
        print_indent(indent + 1);
        printf("name: %s%s\n", pool_get(strings, name),
               (meta & META_VIS_MASK) == VIS_PUBLIC ? "  [pub]"
             : (meta & META_VIS_MASK) == VIS_INTERNAL ? "  [abstract]" : "");
        if (type_id.idx) {
            print_indent(indent + 1); printf("type:\n");
            dump_ast_node(ast, type_id, indent + 2, strings);
        }
        if (value_id.idx) {
            print_indent(indent + 1); printf("value:\n");
            dump_ast_node(ast, value_id, indent + 2, strings);
        }
        return;
    }

    // Destructure bind: extras = [pattern_node_id, type_id, value_id, meta].
    // Slot 0 is the AST_EXPR_PRODUCT pattern (NOT a StrId — sema reads).
    if (kind == AST_DECL_DESTRUCTURE) {
        uint32_t *extra = &((uint32_t*)ast->extra.data)[data.extra_idx.idx];
        AstNodeId pattern_id = { extra[0] };
        AstNodeId type_id    = { extra[1] };
        AstNodeId value_id   = { extra[2] };
        print_indent(indent + 1); printf("pattern:\n");
        dump_ast_node(ast, pattern_id, indent + 2, strings);
        if (type_id.idx) {
            print_indent(indent + 1); printf("type:\n");
            dump_ast_node(ast, type_id, indent + 2, strings);
        }
        if (value_id.idx) {
            print_indent(indent + 1); printf("value:\n");
            dump_ast_node(ast, value_id, indent + 2, strings);
        }
        return;
    }

    // Lambda: extras = [ret_type, body, effect, param_count, params...].
    if (kind == AST_EXPR_LAMBDA) {
        uint32_t *extra = &((uint32_t*)ast->extra.data)[data.extra_idx.idx];
        AstNodeId ret_type = { extra[0] };
        AstNodeId body = { extra[1] };
        AstNodeId effect = { extra[2] };
        uint32_t param_count = extra[3];
        for (uint32_t i = 0; i < param_count; i++) {
            print_indent(indent + 1); printf("Param %u:\n", i);
            dump_ast_node(ast, (AstNodeId){extra[4+i]}, indent + 2, strings);
        }
        if (effect.idx) {
            print_indent(indent + 1); printf("Effect:\n");
            dump_ast_node(ast, effect, indent + 2, strings);
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
    
    // Fn type: extras = [ret, effect, param_count, params...].
    if (kind == AST_TYPE_FN) {
        uint32_t *extra = &((uint32_t*)ast->extra.data)[data.extra_idx.idx];
        AstNodeId ret = { extra[0] };
        AstNodeId eff = { extra[1] };
        uint32_t pc = extra[2];
        for (uint32_t i = 0; i < pc; i++) {
            print_indent(indent + 1); printf("Param %u:\n", i);
            dump_ast_node(ast, (AstNodeId){extra[3+i]}, indent + 2, strings);
        }
        if (eff.idx) {
            print_indent(indent + 1); printf("Effect:\n");
            dump_ast_node(ast, eff, indent + 2, strings);
        }
        if (ret.idx) {
            print_indent(indent + 1); printf("Returns:\n");
            dump_ast_node(ast, ret, indent + 2, strings);
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

    // struct/union: [field_count, field0..]; enum: [variant_count, v0..].
    // Each child is an AST_DECL_VAL (handled by its own case below).
    if (kind == AST_DECL_STRUCT || kind == AST_DECL_UNION ||
        kind == AST_DECL_ENUM) {
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
    
    if (kind == AST_EXPR_INDEX) { // data.bin: lhs=recv, rhs=index
        dump_ast_node(ast, data.bin.lhs, indent + 1, strings);
        dump_ast_node(ast, data.bin.rhs, indent + 1, strings);
        return;
    }

    if (kind == AST_EXPR_SLICE) { // extras [recv, lo, hi]; hi 0 = open
        uint32_t *extra = &((uint32_t*)ast->extra.data)[data.extra_idx.idx];
        dump_ast_node(ast, (AstNodeId){extra[0]}, indent + 1, strings);
        dump_ast_node(ast, (AstNodeId){extra[1]}, indent + 1, strings);
        dump_ast_node(ast, (AstNodeId){extra[2]}, indent + 1, strings);
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

    // op-sort labels shared by handler branches & effect-op signatures.
    static const char *const OP_KIND_NAME[5] =
        { "fn", "ctl", "final ctl", "raw ctl", "val" };

    // Handler: extras = [hdr, effect, initially, return, finally,
    //                    branch_count, (op_sort, name_tok, lambda) x N].
    if (kind == AST_EXPR_HANDLER) {
        uint32_t *extra = &((uint32_t*)ast->extra.data)[data.extra_idx.idx];
        uint32_t hdr = extra[0];
        print_indent(indent + 1);
        printf("flags: named=%d scoped=%d override=%d\n",
               (hdr & 1) != 0, (hdr & 2) != 0, (hdr & 4) != 0);
        AstNodeId eff = { extra[1] }, ini = { extra[2] };
        AstNodeId ret = { extra[3] }, fin = { extra[4] };
        if (eff.idx) { print_indent(indent+1); printf("effect:\n");
                       dump_ast_node(ast, eff, indent + 2, strings); }
        if (ini.idx) { print_indent(indent+1); printf("initially:\n");
                       dump_ast_node(ast, ini, indent + 2, strings); }
        if (ret.idx) { print_indent(indent+1); printf("return:\n");
                       dump_ast_node(ast, ret, indent + 2, strings); }
        if (fin.idx) { print_indent(indent+1); printf("finally:\n");
                       dump_ast_node(ast, fin, indent + 2, strings); }
        uint32_t bc = extra[5];
        for (uint32_t i = 0; i < bc; i++) {
            uint32_t sort = extra[6 + i*3];
            print_indent(indent + 1);
            printf("op[%s]:\n", sort < 5 ? OP_KIND_NAME[sort] : "?");
            dump_ast_node(ast, (AstNodeId){extra[6 + i*3 + 2]},
                          indent + 2, strings);
        }
        return;
    }

    if (kind == AST_EXPR_HANDLE) { // bin: lhs=handler, rhs=action
        print_indent(indent + 1); printf("action:\n");
        dump_ast_node(ast, data.bin.rhs, indent + 2, strings);
        dump_ast_node(ast, data.bin.lhs, indent + 1, strings);
        return;
    }

    if (kind == AST_EXPR_MASK) { // bin: lhs=effect-row, rhs=inner
        if (data.bin.lhs.idx) {
            print_indent(indent + 1); printf("effect:\n");
            dump_ast_node(ast, data.bin.lhs, indent + 2, strings);
        }
        dump_ast_node(ast, data.bin.rhs, indent + 1, strings);
        return;
    }

    // Effect row: extras = [flags, tail_strid, label_count, label0..].
    if (kind == AST_EXPR_EFFECT_ROW) {
        uint32_t *extra = &((uint32_t*)ast->extra.data)[data.extra_idx.idx];
        uint32_t flags = extra[0];
        StrId tail = { extra[1] };
        uint32_t lc = extra[2];
        print_indent(indent + 1);
        const char *head = (lc == 0 && !(flags & 1)) ? "total" : "labels";
        if (!(flags & 1))            printf("%s\n", head);
        else if (tail.idx)           printf("%s (open ..%s)\n", head,
                                            pool_get(strings, tail));
        else                         printf("%s (open ...)\n", head);
        for (uint32_t i = 0; i < lc; i++)
            dump_ast_node(ast, (AstNodeId){extra[3 + i]}, indent + 2, strings);
        return;
    }

    // Effect decl: extras = [hdr, in_type, tparam_count, tp..,
    //                        sig_count, (op_sort, name_tok, sig) x N].
    if (kind == AST_DECL_EFFECT) {
        uint32_t *extra = &((uint32_t*)ast->extra.data)[data.extra_idx.idx];
        AstNodeId in_t = { extra[1] };
        if (in_t.idx) { print_indent(indent+1); printf("in:\n");
                        dump_ast_node(ast, in_t, indent + 2, strings); }
        uint32_t tpc = extra[2];
        for (uint32_t i = 0; i < tpc; i++) {
            print_indent(indent + 1); printf("typaram %u:\n", i);
            dump_ast_node(ast, (AstNodeId){extra[3 + i]}, indent + 2, strings);
        }
        uint32_t sc_at = 3 + tpc;
        uint32_t sc = extra[sc_at];
        for (uint32_t i = 0; i < sc; i++) {
            uint32_t sort = extra[sc_at + 1 + i*3];
            print_indent(indent + 1);
            printf("sig[%s]:\n", sort < 5 ? OP_KIND_NAME[sort] : "?");
            dump_ast_node(ast, (AstNodeId){extra[sc_at + 1 + i*3 + 2]},
                          indent + 2, strings);
        }
        return;
    }
}

void ast_dump_module(ASTStore *ast, Vec *top_level_index, StringPool *strings) {
    if (top_level_index && top_level_index->count > 0) {
        printf("Top-Level Index:\n");
        for (size_t i = 0; i < top_level_index->count; i++) {
            TopLevelEntry *e = (TopLevelEntry*)vec_get(top_level_index, i);
            uint8_t vis = e->meta & META_VIS_MASK;
            printf("  - %s (Node: %u, Vis: %s, AstId: %08x)\n",
                pool_get(strings, e->name), e->node.idx,
                vis == VIS_PUBLIC ? "pub" : vis == VIS_INTERNAL ? "abstract" : "private",
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
