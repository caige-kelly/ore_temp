#include "ids.h"
#include "../db.h"
#include "../workspace/module_info.h"

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

    // Arena-backed fixed-capacity: 256 frames covers real-world nesting with
    // plenty of breathing room, and overflow asserts at the call site
    // rather than triggering a silent realloc that would invalidate any
    // QueryFrame pointer held across a nested db_query_begin. The query
    // engine assumes the stack never relocates — see query.c.
    vec_init_in_arena(&s->query_stack, &s->arena, 256, sizeof(struct QueryFrame));
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

// Allocate a fresh ModuleInfo in db.arena (pointer-stable), register in
// db.modules, return ModuleId. Identity is set; per-module storage is
// untouched — caller (or module_info_init) initializes that. Embedded
// slot fields are zero-init (QUERY_EMPTY) via arena_alloc's zeroing.
ModuleId db_alloc_module(struct db *s) {
    uint32_t idx = (uint32_t)s->modules.count;
    struct ModuleInfo *mod = arena_alloc(&s->arena, sizeof(struct ModuleInfo));
    mod->id = (ModuleId){.idx = idx};
    vec_push(&s->modules, &mod);
    return mod->id;
}

struct ModuleInfo *db_get_module(struct db *s, ModuleId mid) {
    if (!module_id_valid(mid) || mid.idx >= s->modules.count) return NULL;
    struct ModuleInfo **slot = (struct ModuleInfo **)vec_get(&s->modules, mid.idx);
    return slot ? *slot : NULL;
}

ModuleId db_module_for_file(struct db *s, FileId file) {
    if (!file_id_valid(file)) return MODULE_ID_NONE;
    // Skip idx 0 — NONE sentinel.
    for (size_t i = 1; i < s->modules.count; i++) {
        struct ModuleInfo **slot = (struct ModuleInfo **)vec_get(&s->modules, i);
        if (!slot || !*slot) continue;
        if (file_id_eq((*slot)->file, file)) return (*slot)->id;
    }
    return MODULE_ID_NONE;
}

// FNV-1a 64-bit over a byte buffer. Used for source content hashing so
// the LSP can detect "nothing actually changed" without re-parsing.
// Inline rather than depending on query_engine.h for one helper.
static uint64_t source_fnv1a(const char *data, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;   // FNV offset basis (64-bit)
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)data[i];
        h *= 0x100000001b3ULL;             // FNV prime (64-bit)
    }
    return h;
}

SourceId db_alloc_source(struct db *s,
                         const char *path, size_t path_len,
                         const char *text, size_t text_len) {
    uint32_t idx = (uint32_t)s->sources.count;

    StrId path_id = pool_intern(&s->strings, path, path_len);

    // Copy text into db.arena. +1 for a trailing NUL so the lexer can
    // treat it as a C string when convenient. arena_alloc_raw gives us
    // uninitialized bytes; we memcpy and place the NUL ourselves.
    char *text_copy = (char *)arena_alloc_raw(&s->arena, text_len + 1);
    if (text_len) memcpy(text_copy, text, text_len);
    text_copy[text_len] = '\0';

    struct Source src = {
        .file_id  = file_id_make_physical(idx),
        .path     = path_id,
        .text     = text_copy,
        .text_len = (uint32_t)text_len,
        .hash     = source_fnv1a(text, text_len),
        .version  = 1,
    };
    vec_push(&s->sources, &src);
    return (SourceId){.idx = idx};
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
