// HIR dumper for `--dump-hir`. See dump.h for context.

#include "dump.h"

#include <stdio.h>

#include "hir.h"
#include "../sema/sema.h"
#include "../sema/type.h"
#include "../name_resolution/name_resolution.h"
#include "../common/stringpool.h"
#include "../compiler/compiler.h"

static void print_indent(int n) {
    for (int i = 0; i < n; i++) printf("  ");
}

static const char* decl_name(struct Sema* s, struct Decl* d) {
    if (!d || !s) return "?";
    return pool_get(s->pool, d->name.string_id, 0);
}

static const char* type_str(struct Sema* s, struct Type* t, char* buf, size_t cap) {
    if (!t) return "?";
    return sema_type_display_name(s, t, buf, cap);
}

static void dump_instr(struct Sema* s, struct HirInstr* h, int indent);

static void dump_block(struct Sema* s, Vec* block, int indent) {
    if (!block) return;
    for (size_t i = 0; i < block->count; i++) {
        struct HirInstr** hp = (struct HirInstr**)vec_get(block, i);
        if (hp && *hp) dump_instr(s, *hp, indent);
    }
}

static const char* token_kind_name(enum TokenKind k) {
    return token_kind_to_str(k);
}

static const char* unary_op_name(enum UnaryOp op) {
    switch (op) {
        case unary_Ref: return "&";
        case unary_Deref: return "*";
        case unary_Ptr: return "^";
        case unary_ManyPtr: return "[^]";
        case unary_Neg: return "-";
        case unary_Not: return "!";
        case unary_BitNot: return "~";
        case unary_Const: return "const";
        case unary_Optional: return "?";
        case unary_Inc: return "++";
    }
    return "?";
}

static void dump_instr(struct Sema* s, struct HirInstr* h, int indent) {
    if (!h) return;
    char tbuf[128];
    print_indent(indent);
    printf("%s : %s", hir_kind_str(h->kind),
        type_str(s, h->type, tbuf, sizeof(tbuf)));

    switch (h->kind) {
        case HIR_REF:
            if (h->ref.decl) {
                printf("  -> %s", decl_name(s, h->ref.decl));
            }
            printf("\n");
            return;
        case HIR_BIND:
            if (h->bind.decl) {
                printf("  '%s'", decl_name(s, h->bind.decl));
            }
            printf("\n");
            if (h->bind.init) {
                print_indent(indent + 1); printf("init:\n");
                dump_instr(s, h->bind.init, indent + 2);
            }
            return;
        case HIR_BIN:
            printf("  %s\n", token_kind_name(h->bin.op));
            print_indent(indent + 1); printf("left:\n");
            dump_instr(s, h->bin.left, indent + 2);
            print_indent(indent + 1); printf("right:\n");
            dump_instr(s, h->bin.right, indent + 2);
            return;
        case HIR_UNARY:
            printf("  %s%s\n",
                h->unary.postfix ? "postfix " : "",
                unary_op_name(h->unary.op));
            print_indent(indent + 1); printf("operand:\n");
            dump_instr(s, h->unary.operand, indent + 2);
            return;
        case HIR_ASSIGN:
            printf("\n");
            print_indent(indent + 1); printf("target:\n");
            dump_instr(s, h->assign.target, indent + 2);
            print_indent(indent + 1); printf("value:\n");
            dump_instr(s, h->assign.value, indent + 2);
            return;
        case HIR_FIELD:
            printf("  .%s\n", h->field.field_name_id
                ? pool_get(s->pool, h->field.field_name_id, 0) : "?");
            print_indent(indent + 1); printf("object:\n");
            dump_instr(s, h->field.object, indent + 2);
            return;
        case HIR_INDEX:
            printf("\n");
            print_indent(indent + 1); printf("object:\n");
            dump_instr(s, h->index.object, indent + 2);
            print_indent(indent + 1); printf("index:\n");
            dump_instr(s, h->index.index, indent + 2);
            return;
        case HIR_PRODUCT:
            printf("\n");
            if (h->product.fields) {
                for (size_t i = 0; i < h->product.fields->count; i++) {
                    struct HirInstr** fp = (struct HirInstr**)
                        vec_get(h->product.fields, i);
                    print_indent(indent + 1); printf("[%zu]:\n", i);
                    if (fp && *fp) dump_instr(s, *fp, indent + 2);
                }
            }
            return;
        case HIR_ARRAY_LIT:
            printf("\n");
            if (h->array_lit.size) {
                print_indent(indent + 1); printf("size:\n");
                dump_instr(s, h->array_lit.size, indent + 2);
            }
            if (h->array_lit.initializer) {
                print_indent(indent + 1); printf("init:\n");
                dump_instr(s, h->array_lit.initializer, indent + 2);
            }
            return;
        case HIR_ENUM_REF:
            printf("  .%s\n", h->enum_ref.variant_name_id
                ? pool_get(s->pool, h->enum_ref.variant_name_id, 0) : "?");
            return;
        case HIR_IF:
            printf("\n");
            print_indent(indent + 1); printf("cond:\n");
            dump_instr(s, h->if_instr.condition, indent + 2);
            print_indent(indent + 1); printf("then:\n");
            dump_block(s, h->if_instr.then_block, indent + 2);
            if (h->if_instr.else_block) {
                print_indent(indent + 1); printf("else:\n");
                dump_block(s, h->if_instr.else_block, indent + 2);
            }
            return;
        case HIR_LOOP:
            printf("\n");
            if (h->loop.init) {
                print_indent(indent + 1); printf("init:\n");
                dump_instr(s, h->loop.init, indent + 2);
            }
            if (h->loop.condition) {
                print_indent(indent + 1); printf("cond:\n");
                dump_instr(s, h->loop.condition, indent + 2);
            }
            if (h->loop.step) {
                print_indent(indent + 1); printf("step:\n");
                dump_instr(s, h->loop.step, indent + 2);
            }
            print_indent(indent + 1); printf("body:\n");
            dump_block(s, h->loop.body_block, indent + 2);
            return;
        case HIR_SWITCH:
            printf("\n");
            print_indent(indent + 1); printf("scrutinee:\n");
            dump_instr(s, h->switch_instr.scrutinee, indent + 2);
            if (h->switch_instr.arms) {
                for (size_t i = 0; i < h->switch_instr.arms->count; i++) {
                    struct HirSwitchArm** ap = (struct HirSwitchArm**)
                        vec_get(h->switch_instr.arms, i);
                    if (!ap || !*ap) continue;
                    print_indent(indent + 1); printf("arm[%zu]:\n", i);
                    if ((*ap)->patterns) {
                        for (size_t j = 0; j < (*ap)->patterns->count; j++) {
                            struct HirInstr** pp = (struct HirInstr**)
                                vec_get((*ap)->patterns, j);
                            print_indent(indent + 2); printf("pattern:\n");
                            if (pp && *pp) dump_instr(s, *pp, indent + 3);
                        }
                    }
                    print_indent(indent + 2); printf("body:\n");
                    dump_block(s, (*ap)->body_block, indent + 3);
                }
            }
            return;
        case HIR_RETURN:
            printf("\n");
            if (h->return_instr.value) {
                print_indent(indent + 1); printf("value:\n");
                dump_instr(s, h->return_instr.value, indent + 2);
            }
            return;
        case HIR_DEFER:
            printf("\n");
            if (h->defer.value) {
                print_indent(indent + 1); printf("value:\n");
                dump_instr(s, h->defer.value, indent + 2);
            }
            return;
        case HIR_TYPE_VALUE: {
            char tt[128];
            printf("  = %s\n", type_str(s, h->type_value.type, tt, sizeof(tt)));
            return;
        }
        case HIR_ASM:
            printf("  \"%s\"\n", h->asm_instr.string_id
                ? pool_get(s->pool, h->asm_instr.string_id, 0) : "");
            return;
        case HIR_CONST:
        case HIR_BREAK:
        case HIR_CONTINUE:
        case HIR_ERROR:
        default:
            printf("\n");
            return;
    }
}

static void dump_fn(struct Sema* s, struct HirFn* fn) {
    if (!fn) return;
    char rbuf[128];
    const char* name = decl_name(s, fn->source);
    printf("Fn %s -> %s\n",
        name ? name : "?",
        type_str(s, fn->ret_type, rbuf, sizeof(rbuf)));
    dump_block(s, fn->body_block, 1);
}

void dump_hir(struct Sema* s) {
    if (!s || !s->compiler || !s->compiler->modules) return;
    printf("\n=== hir ===\n");
    Vec* modules = s->compiler->modules;
    for (size_t i = 0; i < modules->count; i++) {
        struct Module** mp = (struct Module**)vec_get(modules, i);
        struct Module* mod = mp ? *mp : NULL;
        if (!mod) continue;
        struct HirModule* hmod = (struct HirModule*)hashmap_get(
            &s->module_hir, (uint64_t)(uintptr_t)mod);
        if (!hmod || !hmod->functions) continue;
        for (size_t j = 0; j < hmod->functions->count; j++) {
            struct HirFn** fp = (struct HirFn**)vec_get(hmod->functions, j);
            if (fp && *fp) dump_fn(s, *fp);
        }
    }
}
