// Engine compaction — runs at db_request_end.
//
// Two complementary jobs:
//
//   1. Shared-pool mark-and-copy (existing pattern, folded from compact.c).
//      Compacts body_scope_rows + body_scope_binds + scopes.decl_pool —
//      append-only pools that accumulate dead ranges as queries re-run.
//
//   2. Orphan slot reclamation (NEW for the engine rewrite — addresses
//      Phase 0 Bug 3, the orphan-DefId diag leak). Walks per-kind slot
//      tables; any slot whose verified_rev fell behind by more than
//      ENGINE_ORPHAN_THRESHOLD revisions is reclaimed:
//        - state ← QUERY_EMPTY, fingerprint ← FINGERPRINT_NONE
//        - deps + dep_index zeroed (engine arena owns them; just drop refs)
//        - diag unit cleared via db_diags_clear
//        - HashMap-routed kinds: routing entry removed
//        - stats[kind].orphan_reclaimed bumped
//
// Both jobs gated on threshold (count > last_compacted * GROWTH_FACTOR &&
// count > MIN_THRESHOLD) — amortized so short batch compiles don't pay
// the mark-and-copy cost.

#define ORE_ENGINE_PRIVATE
#include "engine.h"
#include "engine_internal.h"

#include "../db.h"
#include "../diag/diag.h"

#include "../../support/data_structure/hashmap.h"
#include "../../support/data_structure/vec.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define COMPACT_GROWTH_FACTOR 2

// ENGINE_ORPHAN_THRESHOLD is defined in engine_internal.h.

// ----------------------------------------------------------------------------
// Orphan reclamation for a single slot
// ----------------------------------------------------------------------------

static void reclaim_slot(db_query_ctx *ctx, QueryKind kind, uint64_t key,
                        QuerySlotHot *slot, QuerySlotCold *cold) {
    struct db *s = (struct db *)ctx;
    slot->state              = QUERY_EMPTY;
    slot->fingerprint        = FINGERPRINT_NONE;
    slot->durability         = DUR_LOW;
    // deps + dep_index: the Vec/HashMap STRUCTS live in db.arena (reclaimed
    // at db_free), but their internal data buffers are malloc-backed (see
    // vec_grow / hashmap insert paths). Without explicit free here, every
    // orphan reclamation leaks the data buffer. H15 fix.
    if (slot->deps)      vec_free(slot->deps);
    if (slot->dep_index) hashmap_free(slot->dep_index);
    slot->deps               = NULL;
    slot->dep_index          = NULL;
    cold->computed_rev       = 0;
    cold->stamped_rev        = 0;
    db_diags_clear(s, kind, key);
    s->query_stats[(int)kind].orphan_reclaimed++;
}

// ----------------------------------------------------------------------------
// Per-kind orphan walk
//
// Each clause walks the kind's slot storage and reclaims orphan rows.
// HashMap-routed kinds: walking the routing HashMap is fine — we don't
// mutate it during the walk; reclaim_slot just zeros the slot row.
// Vec-indexed kinds: iterate the column directly.
// ----------------------------------------------------------------------------

// Walk a Vec-indexed slot column (now PagedVec-backed post-A2). Routing
// key is stamped into cold->routing_key at first use (db_query_begin
// EMPTY path / db_query_stamp_direct), so reclaim reads it directly —
// no need for a key_from_row callback. This is the SAME pattern as
// HashMap-routed kinds; H9 unified them.
static void reclaim_vec_kind(db_query_ctx *ctx, QueryKind kind,
                             PagedVec *hot_vec, PagedVec *cold_vec,
                             uint64_t threshold) {
    size_t n = paged_count(hot_vec);
    for (uint32_t row = 0; row < n; row++) {
        QuerySlotHot *slot = (QuerySlotHot *)paged_get(hot_vec, row);
        if (slot->state == QUERY_EMPTY) continue;
        if (slot->verified_rev >= threshold) continue;
        QuerySlotCold *cold = (QuerySlotCold *)paged_get(cold_vec, row);
        reclaim_slot(ctx, kind, cold->routing_key, slot, cold);
    }
}

// Walk a HashMap-routed kind. For each (routing_key → row), check the
// slot's verified_rev; reclaim if orphan. Note: we don't remove the
// HashMap entry today — the row stays at QUERY_EMPTY and will be
// re-allocated on next first-call. (Removing the HashMap entry while
// iterating is fragile; deferred to a future pass.)
//
// The slot's real routing key is stamped into cold->routing_key at
// db_query_slot_alloc time, so reclaim can pass the correct (kind, key)
// pair to db_diags_clear without reverse-walking the HashMap.
static void reclaim_hashmap_kind(db_query_ctx *ctx, QueryKind kind,
                                 PagedVec *hot_vec, PagedVec *cold_vec,
                                 HashMap *route_map, uint64_t threshold) {
    (void)route_map; // unused in this scan; we walk the slot column directly
    size_t n = paged_count(hot_vec);
    for (uint32_t row = 0; row < n; row++) {
        QuerySlotHot *slot = (QuerySlotHot *)paged_get(hot_vec, row);
        if (slot->state == QUERY_EMPTY) continue;
        if (slot->verified_rev >= threshold) continue;
        QuerySlotCold *cold = (QuerySlotCold *)paged_get(cold_vec, row);
        reclaim_slot(ctx, kind, cold->routing_key, slot, cold);
    }
}

// ----------------------------------------------------------------------------
// Per-DefId type-slot reclamation
//
// TYPE_OF_DECL / FN_SIGNATURE / INFER_BODY / BODY_SCOPES live in per-kind
// columns indexed by `db.defs.kind_row[def.idx]`. There's no separate
// routing map — the key IS the DefId. Walks db.defs.kinds[] once and
// routes each non-NONE def to its slot rows.
//
// `routing_key` for db_diags_clear: just `def.idx` (Vec-indexed by DefId,
// so the slot's cold routing_key stays zero; we know the key from the walk).
// ----------------------------------------------------------------------------

// Per-kind "free embedded heap" hook. Called BEFORE reclaim_slot
// zeroes the slot, for kinds whose result struct contains a HashMap
// or other heap-owning field. Without this, reclamation leaks the
// HashMap that was folded into the result by H11.
//
// Kinds without heap-owning result columns are no-ops here.
static void free_type_slot_result_heap(struct db *s, QueryKind kind,
                                       DefKind k, uint32_t row) {
    switch (kind) {
    case QUERY_TYPE_OF_DECL:
        // VARIABLE/CONSTANT/STRUCT carry a NodeTypesRange embedded in
        // their type_result struct. Free its HashMap before reclaim.
        switch (k) {
        case KIND_STRUCT:
            if (row < paged_count(&s->structs.type_result))
                hashmap_free(&((StructType *)paged_get(&s->structs.type_result, row))->field_node_types.types);
            break;
        case KIND_VARIABLE:
            if (row < paged_count(&s->variables.type_result))
                hashmap_free(&((VariableType *)paged_get(&s->variables.type_result, row))->value_node_types.types);
            break;
        case KIND_CONSTANT:
            if (row < paged_count(&s->constants.type_result))
                hashmap_free(&((ConstantType *)paged_get(&s->constants.type_result, row))->value_node_types.types);
            break;
        default: break;  // FN/UNION/ENUM/EFFECT/HANDLER have flat IpIndex
        }
        break;
    case QUERY_FN_SIGNATURE:
        if (row < paged_count(&s->fns.signature_result))
            hashmap_free(&((FnSignature *)paged_get(&s->fns.signature_result, row))->node_types.types);
        break;
    case QUERY_INFER_BODY:
        if (row < paged_count(&s->fns.body_node_types))
            hashmap_free(&((NodeTypesRange *)paged_get(&s->fns.body_node_types, row))->types);
        break;
    case QUERY_BODY_SCOPES:
        if (row < paged_count(&s->fns.body))
            hashmap_free(&((FnBody *)paged_get(&s->fns.body, row))->scope_map);
        break;
    default: break;
    }
}

static void reclaim_one_type_slot(db_query_ctx *ctx, QueryKind kind,
                                  PagedVec *hot, PagedVec *cold, uint32_t row,
                                  uint64_t def_key, uint64_t threshold,
                                  DefKind def_kind) {
    if (row >= paged_count(hot)) return;
    QuerySlotHot *slot = (QuerySlotHot *)paged_get(hot, row);
    if (slot->state == QUERY_EMPTY) return;
    if (slot->verified_rev >= threshold) return;
    QuerySlotCold *c = (QuerySlotCold *)paged_get(cold, row);
    // Free embedded heap (HashMaps inside result structs) before
    // reclaim_slot zeroes the slot.
    free_type_slot_result_heap((struct db *)ctx, kind, def_kind, row);
    reclaim_slot(ctx, kind, def_key, slot, c);
}

static void reclaim_type_slots(db_query_ctx *ctx, uint64_t threshold) {
    struct db *s = (struct db *)ctx;
    uint32_t n_defs = (uint32_t)s->defs.kinds.count;
    for (uint32_t i = 1; i < n_defs; i++) {  // skip DEF_ID_NONE
        DefKind k = *(DefKind *)vec_get(&s->defs.kinds, i);
        if (k == KIND_NONE) continue;
        uint32_t row = *(uint32_t *)vec_get(&s->defs.kind_row, i);
        uint64_t def_key = (uint64_t)i;

        // TYPE_OF_DECL — route to the per-kind table.
        PagedVec *t_hot = NULL, *t_cold = NULL;
        switch (k) {
        case KIND_FUNCTION:  t_hot = &s->fns.slot_type_hot;       t_cold = &s->fns.slot_type_cold;       break;
        case KIND_STRUCT:    t_hot = &s->structs.slot_type_hot;   t_cold = &s->structs.slot_type_cold;   break;
        case KIND_UNION:     t_hot = &s->unions.slot_type_hot;    t_cold = &s->unions.slot_type_cold;    break;
        case KIND_ENUM:      t_hot = &s->enums.slot_type_hot;     t_cold = &s->enums.slot_type_cold;     break;
        case KIND_EFFECT:    t_hot = &s->effects.slot_type_hot;   t_cold = &s->effects.slot_type_cold;   break;
        case KIND_HANDLER:   t_hot = &s->handlers.slot_type_hot;  t_cold = &s->handlers.slot_type_cold;  break;
        case KIND_VARIABLE:  t_hot = &s->variables.slot_type_hot; t_cold = &s->variables.slot_type_cold; break;
        case KIND_CONSTANT:  t_hot = &s->constants.slot_type_hot; t_cold = &s->constants.slot_type_cold; break;
        default: continue;
        }
        reclaim_one_type_slot(ctx, QUERY_TYPE_OF_DECL, t_hot, t_cold, row, def_key, threshold, k);

        // FN_SIGNATURE / INFER_BODY / BODY_SCOPES — KIND_FUNCTION only.
        if (k == KIND_FUNCTION) {
            reclaim_one_type_slot(ctx, QUERY_FN_SIGNATURE,
                                  &s->fns.slot_signature_hot,
                                  &s->fns.slot_signature_cold,
                                  row, def_key, threshold, k);
            reclaim_one_type_slot(ctx, QUERY_INFER_BODY,
                                  &s->fns.slot_infer_hot,
                                  &s->fns.slot_infer_cold,
                                  row, def_key, threshold, k);
            reclaim_one_type_slot(ctx, QUERY_BODY_SCOPES,
                                  &s->fns.slot_body_scopes_hot,
                                  &s->fns.slot_body_scopes_cold,
                                  row, def_key, threshold, k);
        }
    }
}

uint64_t db_engine_reclaim_orphans(db_query_ctx *ctx, uint64_t threshold_rev) {
    struct db *s = (struct db *)ctx;
    uint64_t before = 0;
    for (int k = 0; k < QUERY_KIND_COUNT; k++) {
        before += s->query_stats[k].orphan_reclaimed;
    }

    // Vec-indexed kinds.
    reclaim_vec_kind(ctx, QUERY_FILE_AST,
                     &s->files.slots_ast_hot, &s->files.slots_ast_cold,
                     threshold_rev);
    reclaim_vec_kind(ctx, QUERY_FILE_IMPORTS,
                     &s->files.slots_file_imports_hot,
                     &s->files.slots_file_imports_cold,
                     threshold_rev);
    reclaim_vec_kind(ctx, QUERY_NAMESPACE_SCOPES,
                     &s->namespaces.slots_exports_hot,
                     &s->namespaces.slots_exports_cold,
                     threshold_rev);
    reclaim_vec_kind(ctx, QUERY_NAMESPACE_TYPE,
                     &s->namespaces.slots_namespace_type_hot,
                     &s->namespaces.slots_namespace_type_cold,
                     threshold_rev);

    // HashMap-routed kinds.
    reclaim_hashmap_kind(ctx, QUERY_DECL_AST,
                         &s->decl_ast.slots_hot, &s->decl_ast.slots_cold,
                         &s->decl_ast_cache, threshold_rev);
    reclaim_hashmap_kind(ctx, QUERY_TOP_LEVEL_ENTRY,
                         &s->top_level_entry.slots_hot,
                         &s->top_level_entry.slots_cold,
                         &s->top_level_entry_cache, threshold_rev);
    reclaim_hashmap_kind(ctx, QUERY_DEF_IDENTITY,
                         &s->def_identity.slots_hot, &s->def_identity.slots_cold,
                         &s->def_by_identity, threshold_rev);
    reclaim_hashmap_kind(ctx, QUERY_RESOLVE_REF,
                         &s->resolve_ref.slots_hot, &s->resolve_ref.slots_cold,
                         &s->resolve_ref_cache, threshold_rev);

    // Per-DefId type slots (TYPE_OF_DECL, FN_SIGNATURE, INFER_BODY, BODY_SCOPES).
    reclaim_type_slots(ctx, threshold_rev);

    uint64_t after = 0;
    for (int k = 0; k < QUERY_KIND_COUNT; k++) {
        after += s->query_stats[k].orphan_reclaimed;
    }
    return after - before;
}

// ----------------------------------------------------------------------------
// db_engine_deep_free — shutdown leak fix (H22)
//
// At db teardown, every non-EMPTY slot still owns malloc-backed deps,
// dep_index, and (for some kinds) HashMaps embedded in its result
// struct. Without this walk the X-macro-driven Vec teardown in db_free
// only releases the column's own buffer — every row's per-slot heap
// would leak.
//
// Implementation: piggyback on db_engine_reclaim_orphans with
// threshold = UINT64_MAX. The reclaim path already does the right
// per-slot cleanup (vec_free deps + hashmap_free dep_index in
// reclaim_slot; free_type_slot_result_heap in reclaim_one_type_slot for
// per-kind embedded HashMaps). With threshold = UINT64_MAX, every
// non-EMPTY slot's `verified_rev < threshold` is trivially true (the
// 31-bit revision field caps at 2^31; UINT64_MAX is far above), so
// the reclaim walks free everything.
// ----------------------------------------------------------------------------

void db_engine_deep_free(db_query_ctx *ctx) {
    (void)db_engine_reclaim_orphans(ctx, UINT64_MAX);
}

// ----------------------------------------------------------------------------
// Shared-pool mark-and-copy compaction (H19)
//
// Pools that grow append-only across query re-runs:
//   - db.body_scope_rows   : live ranges owned by db.fns.body[*].scope_off/len
//   - db.body_scope_binds  : live ranges owned by db.fns.body[*].bind_off/len
//   - db.scopes.decl_pool  : live ranges owned by db.scopes.decl_lo/len[*]
//
// MARK: walk owners, collect (cell_ptr, old_off, len) for each live entry.
//       Live = the owning slot is DONE; non-DONE slots' (off, len) data is
//       indeterminate after orphan reclamation and must be discarded.
// PLAN: sort ranges by old_off; assign new_off as running prefix sum.
// COPY: allocate fresh pool sized to sum(len); memcpy live ranges in order.
// REWRITE: walk ranges again, write new_off back via cell_ptr.
// SWAP : vec_free the old pool; replace it with the new one.
//
// Trigger (threshold-gated): pool.count > MIN_THRESHOLD &&
//                            pool.count > last_compacted * GROWTH_FACTOR.
// ----------------------------------------------------------------------------

#define ORE_COMPACT_MIN_THRESHOLD   4096u
#define ORE_COMPACT_GROWTH_FACTOR   2u

#include <time.h>

static uint64_t compact_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

typedef struct {
    uint32_t old_off;
    uint32_t new_off;
    uint32_t len;
    void    *cell;   // pointer to the owning column entry (FnBody* or null)
} RangeRemap;

static int cmp_remap_by_old_off(const void *a, const void *b) {
    const RangeRemap *ra = (const RangeRemap *)a;
    const RangeRemap *rb = (const RangeRemap *)b;
    if (ra->old_off < rb->old_off) return -1;
    if (ra->old_off > rb->old_off) return 1;
    return 0;
}

// --- body_scope_{rows,binds}: driven by FnBody column ----------------

static void collect_body_scope_ranges(struct db *s, Vec *out_rows, Vec *out_binds) {
    // Only DONE BODY_SCOPES slots own live FnBody data. Skip everything else
    // — reclaimed/empty/running slots' FnBody contents are stale.
    size_t n = paged_count(&s->fns.body);
    for (size_t row = 0; row < n; row++) {
        QuerySlotHot *slot =
            (QuerySlotHot *)paged_get(&s->fns.slot_body_scopes_hot, row);
        if (slot->state != QUERY_DONE) continue;
        FnBody *fb = (FnBody *)paged_get(&s->fns.body, row);
        if (fb->scope_len > 0) {
            RangeRemap rm = {.old_off = fb->scope_off,
                             .new_off = 0,
                             .len     = fb->scope_len,
                             .cell    = fb};
            vec_push(out_rows, &rm);
        }
        if (fb->bind_len > 0) {
            RangeRemap rm = {.old_off = fb->bind_off,
                             .new_off = 0,
                             .len     = fb->bind_len,
                             .cell    = fb};
            vec_push(out_binds, &rm);
        }
    }
}

// Plan + copy + swap one sub-pool. Field-offset within FnBody tells
// us which field (scope_off / bind_off) to rewrite.
static void compact_one_subpool(Vec *old_pool, Vec *ranges, size_t elem_size,
                                size_t rewrite_field_offset_bytes) {
    if (ranges->count > 0)
        qsort(ranges->data, ranges->count, sizeof(RangeRemap),
              cmp_remap_by_old_off);
    uint32_t new_off_cursor = 0;
    for (size_t i = 0; i < ranges->count; i++) {
        RangeRemap *rm = (RangeRemap *)vec_get(ranges, i);
        rm->new_off = new_off_cursor;
        new_off_cursor += rm->len;
    }
    Vec new_pool;
    vec_init(&new_pool, elem_size);
    vec_reserve(&new_pool, new_off_cursor);
    for (size_t i = 0; i < ranges->count; i++) {
        RangeRemap *rm = (RangeRemap *)vec_get(ranges, i);
        void *src = (char *)old_pool->data + (size_t)rm->old_off * elem_size;
        for (uint32_t j = 0; j < rm->len; j++) {
            vec_push(&new_pool, (char *)src + (size_t)j * elem_size);
        }
    }
    for (size_t i = 0; i < ranges->count; i++) {
        RangeRemap *rm = (RangeRemap *)vec_get(ranges, i);
        uint32_t *field =
            (uint32_t *)((char *)rm->cell + rewrite_field_offset_bytes);
        *field = rm->new_off;
    }
    vec_free(old_pool);
    *old_pool = new_pool;
}

static void compact_body_scope_pools(struct db *s) {
    uint64_t t0 = compact_now_ns();
    uint64_t pre_bytes =
        (uint64_t)s->body_scope_rows.count  * sizeof(ScopeRow) +
        (uint64_t)s->body_scope_binds.count * sizeof(ScopedBind);

    Vec rows, binds;
    vec_init(&rows,  sizeof(RangeRemap));
    vec_init(&binds, sizeof(RangeRemap));
    collect_body_scope_ranges(s, &rows, &binds);

    compact_one_subpool(&s->body_scope_rows,  &rows,  sizeof(ScopeRow),
                        offsetof(FnBody, scope_off));
    compact_one_subpool(&s->body_scope_binds, &binds, sizeof(ScopedBind),
                        offsetof(FnBody, bind_off));

    vec_free(&rows);
    vec_free(&binds);

    s->last_compacted_body_scope_rows_count  = (uint32_t)s->body_scope_rows.count;
    s->last_compacted_body_scope_binds_count = (uint32_t)s->body_scope_binds.count;

    uint64_t post_bytes =
        (uint64_t)s->body_scope_rows.count  * sizeof(ScopeRow) +
        (uint64_t)s->body_scope_binds.count * sizeof(ScopedBind);
    s->compact_stats.n_compactions[0]++;
    s->compact_stats.bytes_reclaimed[0] += (pre_bytes - post_bytes);
    s->compact_stats.total_ns[0] += compact_now_ns() - t0;
}

// --- decl_pool: live = scopes reachable from namespaces.exports ------

static void compact_decl_pool(struct db *s) {
    uint64_t t0 = compact_now_ns();
    uint32_t pre_count = (uint32_t)s->scopes.decl_pool.count;
    size_t   scope_count = s->scopes.parents.count;

    if (scope_count == 0) {
        s->last_compacted_decl_pool_count = (uint32_t)s->scopes.decl_pool.count;
        return;
    }

    // Mark: a parallel bitset over scope ids (1 byte per scope is cheap
    // at typical workspace scale; avoids a HashMap allocation).
    Vec live;
    vec_init(&live, sizeof(uint8_t));
    for (size_t i = 0; i < scope_count; i++) vec_push_zero(&live);

    if (s->primitives_scope.idx < scope_count)
        *(uint8_t *)vec_get(&live, s->primitives_scope.idx) = 1;
    for (size_t i = 0; i < s->namespaces.exports.count; i++) {
        NamespaceScopes *ns =
            (NamespaceScopes *)vec_get(&s->namespaces.exports, i);
        if (ns->internal.idx != SCOPE_ID_NONE.idx &&
            ns->internal.idx < scope_count)
            *(uint8_t *)vec_get(&live, ns->internal.idx) = 1;
        if (ns->exported.idx != SCOPE_ID_NONE.idx &&
            ns->exported.idx < scope_count)
            *(uint8_t *)vec_get(&live, ns->exported.idx) = 1;
    }

    // Copy live ranges in scope-id order; stamp new (lo, len) immediately.
    // Dead scopes get len=0 (slot kept for stable scope-id indexing).
    Vec new_pool;
    vec_init(&new_pool, sizeof(DeclEntry));
    for (size_t i = 0; i < scope_count; i++) {
        uint8_t is_live = *(uint8_t *)vec_get(&live, i);
        if (!is_live) {
            *(uint32_t *)vec_get(&s->scopes.decl_lo, i)  = 0;
            *(uint32_t *)vec_get(&s->scopes.decl_len, i) = 0;
            continue;
        }
        uint32_t old_lo = *(uint32_t *)vec_get(&s->scopes.decl_lo, i);
        uint32_t len    = *(uint32_t *)vec_get(&s->scopes.decl_len, i);
        uint32_t new_lo = (uint32_t)new_pool.count;
        for (uint32_t j = 0; j < len; j++) {
            DeclEntry *de =
                (DeclEntry *)vec_get(&s->scopes.decl_pool, old_lo + j);
            vec_push(&new_pool, de);
        }
        *(uint32_t *)vec_get(&s->scopes.decl_lo, i) = new_lo;
    }
    vec_free(&live);

    vec_free(&s->scopes.decl_pool);
    s->scopes.decl_pool = new_pool;

    uint32_t post_count = (uint32_t)s->scopes.decl_pool.count;
    s->last_compacted_decl_pool_count = post_count;
    s->compact_stats.n_compactions[1]++;
    s->compact_stats.bytes_reclaimed[1] +=
        (uint64_t)(pre_count - post_count) * sizeof(DeclEntry);
    s->compact_stats.total_ns[1] += compact_now_ns() - t0;
}

// --- Trigger ---------------------------------------------------------

static void pools_maybe_compact(struct db *s) {
    uint32_t threshold = s->compact_min_threshold
                             ? s->compact_min_threshold
                             : ORE_COMPACT_MIN_THRESHOLD;
    // body_scope_rows and body_scope_binds share a single trigger; growth
    // in either implies the FnBody walk is worth the cost.
    if (s->body_scope_rows.count > threshold &&
        s->body_scope_rows.count >
            s->last_compacted_body_scope_rows_count * ORE_COMPACT_GROWTH_FACTOR) {
        compact_body_scope_pools(s);
    }
    if (s->scopes.decl_pool.count > threshold &&
        s->scopes.decl_pool.count >
            s->last_compacted_decl_pool_count * ORE_COMPACT_GROWTH_FACTOR) {
        compact_decl_pool(s);
    }
}

// ----------------------------------------------------------------------------

void db_engine_compact(db_query_ctx *ctx) {
    struct db *s = (struct db *)ctx;
    uint64_t cur = db_current_revision(ctx);

    // 1. Orphan slot reclamation — any slot with verified_rev far behind
    //    current is GC'd. Frees its deps+dep_index heap buffers and zeros
    //    the slot. Per-kind free hook in reclaim_one_type_slot also frees
    //    HashMaps embedded in result structs (post-H11).
    uint64_t threshold = cur > ENGINE_ORPHAN_THRESHOLD
                             ? cur - ENGINE_ORPHAN_THRESHOLD
                             : 0;
    if (threshold > 0) {
        (void)db_engine_reclaim_orphans(ctx, threshold);
    }

    // 2. Shared-pool mark-and-copy (H19). Runs AFTER orphan reclamation so
    //    the FnBody/scope owners we walk reflect live state only. Threshold-
    //    gated per-pool so an unchanged pool doesn't pay the cost.
    pools_maybe_compact(s);
}
