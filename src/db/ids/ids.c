#include "ids.h"
#include "../db.h"

#include <assert.h>
#include <string.h>

// Push a zero-filled element at the tail of `v`.
// Sized for the largest id-column element today (struct Source at ~24
// bytes); the assert blows up at compile time if a future column ever
// exceeds the scratch buffer.
static void vec_push_zero(Vec *v) {
    static const uint8_t zero[64] = {0};
    assert(v->element_size <= sizeof(zero));
    vec_push(v, zero);
}

// Initialize every SoA column the database owns, then seat slot 0 of each
// as the NONE sentinel. Matches the convention in ids.h: valid ids start
// at 1, so 0 is always "absent."
void db_ids_init(struct db *s) {
    // Sources column.
    vec_init_in(&s->sources, &s->global_arena, sizeof(struct Source));
    vec_push_zero(&s->sources);

    // Per-module ASTStore pointer table — slot 0 is NULL,
    vec_init_in(&s->module_asts, &s->global_arena, sizeof(ASTStore *));
    ASTStore *none_ast = NULL;
    vec_push(&s->module_asts, &none_ast);

    // Defs columns — parallel-indexed by DefId.idx. db_alloc_def grows
    // every column in lockstep, so DefId(N) means "row N of every column."
    vec_init_in(&s->defs.names,          &s->global_arena, sizeof(StrId));
    vec_init_in(&s->defs.parent_modules, &s->global_arena, sizeof(ModuleId));
    vec_init_in(&s->defs.kinds,          &s->global_arena, sizeof(DefKind));
    vec_init_in(&s->defs.signatures,     &s->global_arena, sizeof(TypeId));
    vec_init_in(&s->defs.values,         &s->global_arena, sizeof(ValueId));
    vec_init_in(&s->defs.effects,        &s->global_arena, sizeof(EffectId));
    vec_init_in(&s->defs.handler,        &s->global_arena, sizeof(HandlerId));
    vec_init_in(&s->defs.file,           &s->global_arena, sizeof(FileId));
    vec_init_in(&s->defs.node,           &s->global_arena, sizeof(AstNodeId));
    vec_init_in(&s->defs.extras,         &s->global_arena, sizeof(AstExtraDataIdx));

    // Seed slot 0 across every column — the DEF_ID_NONE row.
    vec_push_zero(&s->defs.names);
    vec_push_zero(&s->defs.parent_modules);
    vec_push_zero(&s->defs.kinds);
    vec_push_zero(&s->defs.signatures);
    vec_push_zero(&s->defs.values);
    vec_push_zero(&s->defs.effects);
    vec_push_zero(&s->defs.handler);
    vec_push_zero(&s->defs.file);
    vec_push_zero(&s->defs.node);
    vec_push_zero(&s->defs.extras);
}

// Reserve a fresh DefId. Every column grows by one zero-initialized row;
// callers fill in the actual values via direct column writes.
DefId db_alloc_def(struct db *s) {
    uint32_t idx = (uint32_t)s->defs.names.count;

    vec_push_zero(&s->defs.names);
    vec_push_zero(&s->defs.parent_modules);
    vec_push_zero(&s->defs.kinds);
    vec_push_zero(&s->defs.signatures);
    vec_push_zero(&s->defs.values);
    vec_push_zero(&s->defs.effects);
    vec_push_zero(&s->defs.handler);
    vec_push_zero(&s->defs.file);
    vec_push_zero(&s->defs.node);
    vec_push_zero(&s->defs.extras);

    return (DefId){ .idx = idx };
}
