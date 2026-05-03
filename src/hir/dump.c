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

static void dump_instr(struct Sema* s, struct HirInstr* h, int indent) {
    if (!h) return;
    char tbuf[128];
    print_indent(indent);
    printf("%s : %s\n", hir_kind_str(h->kind),
        type_str(s, h->type, tbuf, sizeof(tbuf)));
    // Phase C1.2: kind-specific detail lines land alongside each
    // lowering arm in C1.3+. For now the kind+type line is enough to
    // verify the pipeline works end-to-end.
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
