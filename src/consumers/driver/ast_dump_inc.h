#ifndef AST_DUMP_INC_H
#define AST_DUMP_INC_H

#include "../../parser/ast.h"
#include "../../db/storage/stringpool.h"
#include "../../db/db.h"
#include <stdio.h>

// ast_kind_name is now declared in src/parser/ast.h and defined in
// src/parser/ast.c — single source of truth for the kind→string
// mapping, shared with sema's "kind X not yet implemented" diagnostics.

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
    
    if (kind == AST_EXPR_LIT_INT || kind == AST_EXPR_LIT_FLOAT ||
        kind == AST_EXPR_LIT_STRING || kind == AST_EXPR_LIT_BYTE ||
        kind == AST_EXPR_ASM) {
        printf(" '%s'\n", pool_get(strings, data.string_id));
        return;
    }

    if (kind == AST_EXPR_ENUM_REF) {
        printf(" .%s\n", pool_get(strings, data.string_id));
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
    if (kind == AST_STMT_RETURN ||
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
    // field_count, field0..]; each field AST_INIT_FIELD [name, value].
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

    // If: extras = [cond, then, else]. else 0 = no else branch.
    if (kind == AST_STMT_IF) {
        uint32_t *extra = &((uint32_t*)ast->extra.data)[data.extra_idx.idx];
        AstNodeId cond = { extra[0] };
        AstNodeId then_b = { extra[1] };
        AstNodeId else_b = { extra[2] };
        if (cond.idx) {
            print_indent(indent + 1); printf("cond:\n");
            dump_ast_node(ast, cond, indent + 2, strings);
        }
        if (then_b.idx) {
            print_indent(indent + 1); printf("then:\n");
            dump_ast_node(ast, then_b, indent + 2, strings);
        }
        if (else_b.idx) {
            print_indent(indent + 1); printf("else:\n");
            dump_ast_node(ast, else_b, indent + 2, strings);
        }
        return;
    }

    // Switch: extras = [scrutinee, arm_count, arm0_id..]. Each arm is
    // AST_STMT_SWITCH_ARM with extras = [pat_count, pat0..patN, body_id].
    // Walked inline (arms only ever appear as children of SWITCH).
    if (kind == AST_STMT_SWITCH) {
        uint32_t *extra = &((uint32_t*)ast->extra.data)[data.extra_idx.idx];
        AstNodeId scrutinee = { extra[0] };
        uint32_t arm_count = extra[1];
        print_indent(indent + 1); printf("scrutinee:\n");
        dump_ast_node(ast, scrutinee, indent + 2, strings);
        for (uint32_t i = 0; i < arm_count; i++) {
            AstNodeId arm_id = { extra[2 + i] };
            AstNodeData arm_data = ((AstNodeData*)ast->data.data)[arm_id.idx];
            uint32_t *aex = &((uint32_t*)ast->extra.data)[arm_data.extra_idx.idx];
            uint32_t pc = aex[0];
            print_indent(indent + 1); printf("arm:\n");
            for (uint32_t k = 0; k < pc; k++) {
                print_indent(indent + 2); printf("pat:\n");
                dump_ast_node(ast, (AstNodeId){aex[1 + k]}, indent + 3, strings);
            }
            print_indent(indent + 2); printf("body:\n");
            dump_ast_node(ast, (AstNodeId){aex[1 + pc]}, indent + 3, strings);
        }
        return;
    }

    // Builtin: extras = [name_strid, arg_count, arg0..]. Always print name.
    if (kind == AST_EXPR_BUILTIN) {
        uint32_t *extra = &((uint32_t*)ast->extra.data)[data.extra_idx.idx];
        StrId name = { extra[0] };
        uint32_t argc = extra[1];
        print_indent(indent + 1);
        printf("name: @%s\n", pool_get(strings, name));
        for (uint32_t i = 0; i < argc; i++) {
            print_indent(indent + 1); printf("arg %u:\n", i);
            dump_ast_node(ast, (AstNodeId){extra[2 + i]}, indent + 2, strings);
        }
        return;
    }

    // Loop: extras = [label_strid, init, cond, step, body]. label 0 = unlabeled.
    if (kind == AST_STMT_LOOP) {
        uint32_t *extra = &((uint32_t*)ast->extra.data)[data.extra_idx.idx];
        StrId label = { extra[0] };
        AstNodeId init = { extra[1] };
        AstNodeId cond = { extra[2] };
        AstNodeId step = { extra[3] };
        AstNodeId body = { extra[4] };
        if (label.idx) {
            print_indent(indent + 1);
            printf("label: %s\n", pool_get(strings, label));
        }
        if (init.idx) {
            print_indent(indent + 1); printf("init:\n");
            dump_ast_node(ast, init, indent + 2, strings);
        }
        if (cond.idx) {
            print_indent(indent + 1); printf("cond:\n");
            dump_ast_node(ast, cond, indent + 2, strings);
        }
        if (step.idx) {
            print_indent(indent + 1); printf("step:\n");
            dump_ast_node(ast, step, indent + 2, strings);
        }
        if (body.idx) {
            print_indent(indent + 1); printf("body:\n");
            dump_ast_node(ast, body, indent + 2, strings);
        }
        return;
    }

    // break / continue: data.string_id = target label (0 = innermost).
    if (kind == AST_STMT_BREAK || kind == AST_STMT_CONTINUE) {
        if (data.string_id.idx) {
            print_indent(indent + 1);
            printf("label: %s\n", pool_get(strings, data.string_id));
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

    // struct/union: [field_count, field0..] (children: AST_DECL_FIELD).
    // enum:         [variant_count, v0..]   (children: AST_DECL_VARIANT).
    if (kind == AST_DECL_STRUCT || kind == AST_DECL_UNION ||
        kind == AST_DECL_ENUM) {
        uint32_t *extra = &((uint32_t*)ast->extra.data)[data.extra_idx.idx];
        uint32_t count = extra[0];
        for (uint32_t i=0; i<count; i++) {
            dump_ast_node(ast, (AstNodeId){extra[i+1]}, indent + 1, strings);
        }
        return;
    }

    // Param: extras = [name (0=type-only), type, is_comptime].
    if (kind == AST_DECL_PARAM) {
        uint32_t *extra = &((uint32_t*)ast->extra.data)[data.extra_idx.idx];
        StrId name = { extra[0] };
        AstNodeId type = { extra[1] };
        uint32_t is_comptime = extra[2];
        if (is_comptime) {
            print_indent(indent + 1); printf("[comptime]\n");
        }
        if (name.idx) {
            print_indent(indent + 1);
            printf("name: %s\n", pool_get(strings, name));
        }
        if (type.idx)
            dump_ast_node(ast, type, indent + 1, strings);
        return;
    }

    // Field: extras = [name_strid (0=anon nested), type, vis, fpos (0=auto)].
    if (kind == AST_DECL_FIELD) {
        uint32_t *extra = &((uint32_t*)ast->extra.data)[data.extra_idx.idx];
        StrId name = { extra[0] };
        AstNodeId type = { extra[1] };
        uint32_t vis  = extra[2];
        AstNodeId fpos = { extra[3] };
        if (vis) {
            print_indent(indent + 1); printf("[pub]\n");
        }
        if (name.idx) {
            print_indent(indent + 1);
            printf("name: %s\n", pool_get(strings, name));
        } else {
            print_indent(indent + 1); printf("[anon]\n");
        }
        if (type.idx) dump_ast_node(ast, type, indent + 1, strings);
        if (fpos.idx) {
            print_indent(indent + 1); printf("at:\n");
            dump_ast_node(ast, fpos, indent + 2, strings);
        }
        return;
    }

    // Variant: extras = [name_strid, value (0=auto-numbered)].
    if (kind == AST_DECL_VARIANT) {
        uint32_t *extra = &((uint32_t*)ast->extra.data)[data.extra_idx.idx];
        StrId name = { extra[0] };
        AstNodeId value = { extra[1] };
        if (name.idx) {
            print_indent(indent + 1);
            printf("name: %s\n", pool_get(strings, name));
        }
        if (value.idx) {
            print_indent(indent + 1); printf("=\n");
            dump_ast_node(ast, value, indent + 2, strings);
        }
        return;
    }

    // Init field: extras = [name_strid (0=positional), value].
    if (kind == AST_INIT_FIELD) {
        uint32_t *extra = &((uint32_t*)ast->extra.data)[data.extra_idx.idx];
        StrId name = { extra[0] };
        AstNodeId value = { extra[1] };
        if (name.idx) {
            print_indent(indent + 1);
            printf(".%s\n", pool_get(strings, name));
        }
        if (value.idx) dump_ast_node(ast, value, indent + 1, strings);
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

    // Call: extras = [callee, arg_count, arg0..]. Labels disambiguate
    // callee from args (args were previously rendered at indent+2,
    // making them look like children of the callee path).
    if (kind == AST_EXPR_CALL) {
        uint32_t *extra = &((uint32_t*)ast->extra.data)[data.extra_idx.idx];
        uint32_t argc = extra[1];
        print_indent(indent + 1); printf("callee:\n");
        dump_ast_node(ast, (AstNodeId){extra[0]}, indent + 2, strings);
        for (uint32_t i=0; i<argc; i++) {
            print_indent(indent + 1); printf("arg %u:\n", i);
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

void ast_dump_module(ASTStore *ast, const FileArray *top_level_index,
                     StringPool *strings) {
    if (top_level_index && top_level_index->count > 0) {
        printf("Top-Level Index:\n");
        for (uint32_t i = 0; i < top_level_index->count; i++) {
            const TopLevelEntry *e =
                &((const TopLevelEntry *)top_level_index->data)[i];
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
