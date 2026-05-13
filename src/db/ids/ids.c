#include "ids.h"
#include "../db.h"

#include <assert.h>
#include <string.h>

// =============================================================================
// SoA column initialization.
//
// Every column in db.defs / db.scopes / db.sources / db.modules / db.query_stack
// is a malloc-backed Vec (vec_init). These are long-lived, grow over the
// session, and would leak memory if backed by the arena (every doubling would
// orphan the prior buffer with no reclaim path).
//
// Slot 0 of every "id-indexed" column is reserved as the NONE sentinel — push
// a zero row so DefId(0) / ScopeId(0) / SourceId(0) / ModuleId(0) all map to
// a defined but-empty row. query_stack is a true stack (no NONE convention),
// so it stays count=0 at init.
// =============================================================================

void db_ids_init(struct db *s) {
    /* ---- Sources / modules ----------------------------------------------- */

    vec_init(&s->sources, sizeof(struct Source));
    vec_push_zero(&s->sources);          // SourceId(0) = NONE

    vec_init(&s->modules, sizeof(struct ModuleInfo *));
    struct ModuleInfo *none_mod = NULL;
    vec_push(&s->modules, &none_mod);    // ModuleId(0) = NULL

    /* ---- defs SoA -------------------------------------------------------- */

    // Identity columns (durable across reparses).
    vec_init(&s->defs.names,          sizeof(StrId));
    vec_init(&s->defs.parent_modules, sizeof(ModuleId));
    vec_init(&s->defs.kinds,          sizeof(DefKind));
    vec_init(&s->defs.visibilities,   sizeof(Visibility));
    vec_init(&s->defs.ast_ids,        sizeof(AstId));
    vec_init(&s->defs.owner_scopes,   sizeof(ScopeId));

    // Per-decl durable fingerprint (R6).
    vec_init(&s->defs.durable_fps,    sizeof(Fingerprint));

    // Cached query outputs.
    vec_init(&s->defs.types,          sizeof(IpIndex));
    vec_init(&s->defs.values,         sizeof(IpIndex));
    vec_init(&s->defs.effect_sigs,    sizeof(IpIndex));

    // Per-decl query slot columns.
    vec_init(&s->defs.slots_type,            sizeof(struct QuerySlot));
    vec_init(&s->defs.slots_signature,       sizeof(struct QuerySlot));
    vec_init(&s->defs.slots_is_comptime,     sizeof(struct QuerySlot));
    vec_init(&s->defs.slots_const_eval,      sizeof(struct QuerySlot));

    // Seed slot 0 = DEF_ID_NONE across every defs column.
    vec_push_zero(&s->defs.names);
    vec_push_zero(&s->defs.parent_modules);
    vec_push_zero(&s->defs.kinds);
    vec_push_zero(&s->defs.visibilities);
    vec_push_zero(&s->defs.ast_ids);
    vec_push_zero(&s->defs.owner_scopes);
    vec_push_zero(&s->defs.durable_fps);
    vec_push_zero(&s->defs.types);
    vec_push_zero(&s->defs.values);
    vec_push_zero(&s->defs.effect_sigs);
    vec_push_zero(&s->defs.slots_type);
    vec_push_zero(&s->defs.slots_signature);
    vec_push_zero(&s->defs.slots_is_comptime);
    vec_push_zero(&s->defs.slots_const_eval);

    /* ---- scopes SoA ------------------------------------------------------ */

    vec_init(&s->scopes.parents,          sizeof(ScopeId));
    vec_init(&s->scopes.kinds,            sizeof(ScopeKind));
    vec_init(&s->scopes.owning_modules,   sizeof(ModuleId));
    vec_init(&s->scopes.decl_offsets,     sizeof(uint32_t));
    vec_init(&s->scopes.decl_pool,        sizeof(DeclEntry));
    vec_init(&s->scopes.slots_resolve_ref, sizeof(struct QuerySlot));

    // Seed ScopeId(0) = NONE. decl_offsets needs two entries (start +
    // sentinel-end) for the NONE scope to have a well-formed empty range.
    vec_push_zero(&s->scopes.parents);
    vec_push_zero(&s->scopes.kinds);
    vec_push_zero(&s->scopes.owning_modules);
    vec_push_zero(&s->scopes.decl_offsets);  // start of NONE scope's range
    vec_push_zero(&s->scopes.decl_offsets);  // sentinel: end of NONE scope's range
    vec_push_zero(&s->scopes.slots_resolve_ref);

    /* ---- query stack ----------------------------------------------------- */

    vec_init(&s->query_stack, sizeof(struct QueryFrame));
}

// Reserve a fresh DefId. Every defs column grows by one zero row in
// lockstep so DefId(N) names row N everywhere.
DefId db_alloc_def(struct db *s) {
    uint32_t idx = (uint32_t)s->defs.names.count;

    vec_push_zero(&s->defs.names);
    vec_push_zero(&s->defs.parent_modules);
    vec_push_zero(&s->defs.kinds);
    vec_push_zero(&s->defs.visibilities);
    vec_push_zero(&s->defs.ast_ids);
    vec_push_zero(&s->defs.owner_scopes);
    vec_push_zero(&s->defs.durable_fps);
    vec_push_zero(&s->defs.types);
    vec_push_zero(&s->defs.values);
    vec_push_zero(&s->defs.effect_sigs);
    vec_push_zero(&s->defs.slots_type);
    vec_push_zero(&s->defs.slots_signature);
    vec_push_zero(&s->defs.slots_is_comptime);
    vec_push_zero(&s->defs.slots_const_eval);

    return (DefId){.idx = idx};
}

// Reserve a fresh ScopeId. Every scopes column grows by one zero row;
// the decl_offsets sentinel is updated so the new scope starts with a
// well-formed empty decl range [decl_pool.count, decl_pool.count).
ScopeId db_alloc_scope(struct db *s) {
    uint32_t idx = (uint32_t)s->scopes.parents.count;

    vec_push_zero(&s->scopes.parents);
    vec_push_zero(&s->scopes.kinds);
    vec_push_zero(&s->scopes.owning_modules);
    vec_push_zero(&s->scopes.slots_resolve_ref);

    // The new scope inherits the current decl_pool end as its start.
    // We push one new offset entry (the sentinel that marks the END of
    // the new scope's range). The previous sentinel now serves as the
    // start of the new scope's range. Invariant maintained:
    //     decl_offsets.count == scope_count + 1
    uint32_t end_offset = (uint32_t)s->scopes.decl_pool.count;
    vec_push(&s->scopes.decl_offsets, &end_offset);

    return (ScopeId){.idx = idx};
}

// Free all malloc-backed Vec storage on the database. Called from db_free.
void db_ids_free(struct db *s) {
    vec_free(&s->sources);
    vec_free(&s->modules);

    vec_free(&s->defs.names);
    vec_free(&s->defs.parent_modules);
    vec_free(&s->defs.kinds);
    vec_free(&s->defs.visibilities);
    vec_free(&s->defs.ast_ids);
    vec_free(&s->defs.owner_scopes);
    vec_free(&s->defs.durable_fps);
    vec_free(&s->defs.types);
    vec_free(&s->defs.values);
    vec_free(&s->defs.effect_sigs);
    vec_free(&s->defs.slots_type);
    vec_free(&s->defs.slots_signature);
    vec_free(&s->defs.slots_is_comptime);
    vec_free(&s->defs.slots_const_eval);

    vec_free(&s->scopes.parents);
    vec_free(&s->scopes.kinds);
    vec_free(&s->scopes.owning_modules);
    vec_free(&s->scopes.decl_offsets);
    vec_free(&s->scopes.decl_pool);
    vec_free(&s->scopes.slots_resolve_ref);

    vec_free(&s->query_stack);
}
