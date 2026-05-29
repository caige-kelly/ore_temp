// Engine core — slot lifecycle, dep tracking, routing.
//
// State machine (per slot):
//   EMPTY    → RUNNING (begin) → DONE/ERROR (succeed/fail)
//   DONE     → RUNNING (verify fails → recompute) → DONE
//   ERROR    → RUNNING (verify fails → recompute) → DONE/ERROR
//   RUNNING  → EMPTY (request_end sweep)
//
// Dep recording: per-frame HashMap dedup for O(1) record. Each dep
// carries its own durability tier (Durability dep_dur), recorded for
// future verify-time MIN-over-live-deps refinement; current verify uses
// slot.durability (cached MIN-at-succeed).
//
// Cancellation: db_query_begin returns CANCELED if cancel_requested is
// set, before any slot mutation. DB_QUERY_GUARD honors CANCELED via the
// on_cycle return. Bodies that bail mid-execution leave their slot in
// RUNNING; db_request_end sweeps to EMPTY.

#define ORE_ENGINE_PRIVATE
#include "engine.h"
#include "engine_internal.h"

#include "../db.h"
#include "../diag/diag.h"
#include "../ids/ids.h"

#include "../../support/data_structure/arena.h"
#include "../../support/data_structure/hashmap.h"
#include "../../support/data_structure/vec.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// rev_control bit layout (mirrors db.h documentation):
//   bit 63          : invalidation flag (currently unused)
//   bits 32-62      : current_revision (31 bits)
//   bits 0-31       : request_revision (32 bits)
#define REV_INVALIDATION_MASK (1ULL << 63)
#define REV_CURRENT_MASK      (0x7FFFFFFFULL << 32)
#define REV_REQUEST_MASK      (0xFFFFFFFFULL)
#define REV_CURRENT_SHIFT     32

// ============================================================================
// Static helpers
// ============================================================================

static struct db *db_(db_query_ctx *ctx) { return (struct db *)ctx; }

// Pack a (kind, key_low) into a u64 for the dep_index HashMap. The
// HashMap takes u64 keys; we squeeze both fields in.
static uint64_t dep_index_key(QueryKind kind, uint64_t key) {
    return ((uint64_t)kind << 56) | (key & 0x00FFFFFFFFFFFFFFULL);
}

// Append a QueryDep onto a frame, with O(1) hashmap dedup. If (kind, key)
// already recorded on this frame, refresh its dep_fp in place.
static void record_dep_on_parent(db_query_ctx *ctx, QueryKind child_kind,
                                 uint64_t child_key, Fingerprint child_fp,
                                 Durability child_dur, size_t parent_offset) {
    struct db *s = db_(ctx);
    if (s->query_stack.count <= parent_offset) return;
    QueryFrame *parent = (QueryFrame *)vec_get(
        &s->query_stack, s->query_stack.count - 1 - parent_offset);

    // Lazy-allocate deps + dep_index on first record for this frame.
    if (!parent->deps) {
        parent->deps = arena_alloc(&s->arena, sizeof(Vec));
        vec_init(parent->deps, sizeof(QueryDep));
    }
    if (!parent->dep_index) {
        parent->dep_index = arena_alloc(&s->arena, sizeof(HashMap));
        hashmap_init(parent->dep_index);
    }

    // dep_index is an ADVISORY hint, not authoritative: dep_index_key
    // packs (kind, key) into a u64 and necessarily truncates (8-bit kind
    // OR'd over the key's top bits), so two distinct (kind, key) deps can
    // collide. Confirm the hit's identity before treating it as a dedup
    // — otherwise a collision would overwrite a *different* dep's fp and
    // silently drop it from the graph (→ missed invalidation). On a
    // confirmed collision we append a fresh dep; worst case is a harmless
    // duplicate entry (verify walks both), never a lost one.
    uint64_t ikey = dep_index_key(child_kind, child_key);
    void *existing = hashmap_get(parent->dep_index, ikey);
    bool deduped = false;
    if (existing) {
        uint32_t row = (uint32_t)(uintptr_t)existing;
        QueryDep *dep = (QueryDep *)vec_get(parent->deps, row);
        if (dep->kind == child_kind && dep->key == child_key) {
            dep->dep_fp = child_fp;   // real dedup hit — refresh in place
            dep->dep_dur = child_dur;
            deduped = true;
        }
        // else: ikey collision on a DISTINCT dep — fall through to append.
    }
    if (!deduped) {
        uint32_t row = (uint32_t)parent->deps->count;
        QueryDep d = {.key = child_key, .dep_fp = child_fp,
                      .kind = child_kind, .dep_dur = child_dur};
        vec_push(parent->deps, &d);
        // Only index the first occupant of this ikey; a colliding dep
        // stays unindexed (re-reads of it append again — bounded by
        // re-read count, and collisions require >2^24 entities).
        if (!existing)
            hashmap_put_or_die(parent->dep_index, ikey,
                               (void *)(uintptr_t)row, "engine: dep dedup");
    }

    // Fold child durability into parent's MIN accumulator (set at succeed
    // time as slot->durability).
    if (!parent->dur_set || child_dur < parent->min_input_dur) {
        parent->min_input_dur = child_dur;
    }
    parent->dur_set = true;
}

// Push a frame for a query transitioning to RUNNING. Inherits the slot's
// existing deps Vec so a recompute reuses the buffer (clears its count
// to 0, but retains the malloc backing).
static void query_stack_push(db_query_ctx *ctx, QueryKind kind, uint64_t key,
                             Vec *inherited_deps, HashMap *inherited_dep_index) {
    struct db *s = db_(ctx);
    QueryFrame frame = {
        .kind = kind,
        .key = key,
        .deps = inherited_deps,
        .dep_index = inherited_dep_index,
        .min_input_dur = DUR_HIGH,
        .dur_set = false,
    };
    vec_push(&s->query_stack, &frame);
}

static void query_stack_pop(db_query_ctx *ctx) {
    struct db *s = db_(ctx);
    if (s->query_stack.count > 0) s->query_stack.count--;
}

// ============================================================================
// Routing — (kind, key) → (slots_hot PagedVec*, slots_cold PagedVec*, row).
// Post-A2: all slot columns are PagedVec-backed for pointer stability.
// ============================================================================

bool db_engine_route_slot(db_query_ctx *ctx, QueryKind kind, uint64_t key,
                          QuerySlotHot **out_hot, QuerySlotCold **out_cold) {
    struct db *s = db_(ctx);
    PagedVec *hot_vec = NULL;
    PagedVec *cold_vec = NULL;
    uint32_t row = 0;

    switch (kind) {
    case QUERY_SOURCE_TEXT: {
        // Input kind, keyed by SourceId.idx (dense, no high bit).
        uint32_t local = (uint32_t)key;
        if (local >= s->sources.slots_text_hot.count) return false;
        hot_vec = &s->sources.slots_text_hot;
        cold_vec = &s->sources.slots_text_cold;
        row = local;
        break;
    }
    case QUERY_FILE_AST: {
        FileId f = {.idx = (uint32_t)key};
        uint32_t local = file_id_local(f);
        if (local >= s->files.slots_ast_hot.count) return false;
        hot_vec = &s->files.slots_ast_hot;
        cold_vec = &s->files.slots_ast_cold;
        row = local;
        break;
    }
    case QUERY_LINE_INDEX: {
        FileId f = {.idx = (uint32_t)key};
        uint32_t local = file_id_local(f);
        if (local >= s->files.slots_line_index_hot.count) return false;
        hot_vec = &s->files.slots_line_index_hot;
        cold_vec = &s->files.slots_line_index_cold;
        row = local;
        break;
    }
    case QUERY_FILE_IMPORTS: {
        FileId f = {.idx = (uint32_t)key};
        uint32_t local = file_id_local(f);
        if (local >= s->files.slots_file_imports_hot.count) return false;
        hot_vec = &s->files.slots_file_imports_hot;
        cold_vec = &s->files.slots_file_imports_cold;
        row = local;
        break;
    }
    case QUERY_FILE_SET: {
        // Input kind, keyed by NamespaceId.idx.
        uint32_t nsid = (uint32_t)key;
        if (nsid >= s->namespaces.slots_file_set_hot.count) return false;
        hot_vec = &s->namespaces.slots_file_set_hot;
        cold_vec = &s->namespaces.slots_file_set_cold;
        row = nsid;
        break;
    }
    case QUERY_NAMESPACE_SCOPES: {
        uint32_t nsid = (uint32_t)key;
        if (nsid >= s->namespaces.slots_exports_hot.count) return false;
        hot_vec = &s->namespaces.slots_exports_hot;
        cold_vec = &s->namespaces.slots_exports_cold;
        row = nsid;
        break;
    }
    case QUERY_NAMESPACE_TYPE: {
        uint32_t nsid = (uint32_t)key;
        if (nsid >= s->namespaces.slots_namespace_type_hot.count) return false;
        hot_vec = &s->namespaces.slots_namespace_type_hot;
        cold_vec = &s->namespaces.slots_namespace_type_cold;
        row = nsid;
        break;
    }
    case QUERY_NAMESPACE_ITEMS: {
        uint32_t nsid = (uint32_t)key;
        if (nsid >= s->namespaces.slots_namespace_items_hot.count) return false;
        hot_vec = &s->namespaces.slots_namespace_items_hot;
        cold_vec = &s->namespaces.slots_namespace_items_cold;
        row = nsid;
        break;
    }
    case QUERY_TOP_LEVEL_ENTRY: {
        void *rowp = hashmap_get(&s->top_level_entry_cache, key);
        if (!rowp) return false;
        row = (uint32_t)(uintptr_t)rowp;
        if (row >= s->top_level_entry.slots_hot.count) return false;
        hot_vec = &s->top_level_entry.slots_hot;
        cold_vec = &s->top_level_entry.slots_cold;
        break;
    }
    case QUERY_DEF_IDENTITY: {
        void *rowp = hashmap_get(&s->def_by_identity, key);
        if (!rowp) return false;
        row = (uint32_t)(uintptr_t)rowp;
        if (row >= s->def_identity.slots_hot.count) return false;
        hot_vec = &s->def_identity.slots_hot;
        cold_vec = &s->def_identity.slots_cold;
        break;
    }
    case QUERY_RESOLVE_REF: {
        void *rowp = hashmap_get(&s->resolve_ref_cache, key);
        if (!rowp) return false;
        row = (uint32_t)(uintptr_t)rowp;
        if (row >= s->resolve_ref.slots_hot.count) return false;
        hot_vec = &s->resolve_ref.slots_hot;
        cold_vec = &s->resolve_ref.slots_cold;
        break;
    }
    case QUERY_TYPE_OF_DECL:
    case QUERY_FN_SIGNATURE:
    case QUERY_INFER_BODY:
    case QUERY_BODY_SCOPES: {
        DefId def = {.idx = (uint32_t)key};
        if (def.idx >= s->defs.kinds.count) return false;
        DefKind def_kind = *(DefKind *)vec_get(&s->defs.kinds, def.idx);
        if (def_kind == KIND_NONE) return false;
        row = *(uint32_t *)vec_get(&s->defs.kind_row, def.idx);

        // FN_SIGNATURE / INFER_BODY / BODY_SCOPES are KIND_FUNCTION only.
        if (kind != QUERY_TYPE_OF_DECL && def_kind != KIND_FUNCTION) return false;

        if (kind == QUERY_TYPE_OF_DECL) {
            switch (def_kind) {
            case KIND_FUNCTION:
                hot_vec = &s->fns.slot_type_hot;
                cold_vec = &s->fns.slot_type_cold;
                break;
            case KIND_STRUCT:
                hot_vec = &s->structs.slot_type_hot;
                cold_vec = &s->structs.slot_type_cold;
                break;
            case KIND_UNION:
                hot_vec = &s->unions.slot_type_hot;
                cold_vec = &s->unions.slot_type_cold;
                break;
            case KIND_ENUM:
                hot_vec = &s->enums.slot_type_hot;
                cold_vec = &s->enums.slot_type_cold;
                break;
            case KIND_EFFECT:
                hot_vec = &s->effects.slot_type_hot;
                cold_vec = &s->effects.slot_type_cold;
                break;
            case KIND_HANDLER:
                hot_vec = &s->handlers.slot_type_hot;
                cold_vec = &s->handlers.slot_type_cold;
                break;
            case KIND_VARIABLE:
                hot_vec = &s->variables.slot_type_hot;
                cold_vec = &s->variables.slot_type_cold;
                break;
            case KIND_CONSTANT:
                hot_vec = &s->constants.slot_type_hot;
                cold_vec = &s->constants.slot_type_cold;
                break;
            default:
                return false;
            }
        } else if (kind == QUERY_FN_SIGNATURE) {
            hot_vec = &s->fns.slot_signature_hot;
            cold_vec = &s->fns.slot_signature_cold;
        } else if (kind == QUERY_INFER_BODY) {
            hot_vec = &s->fns.slot_infer_hot;
            cold_vec = &s->fns.slot_infer_cold;
        } else { // QUERY_BODY_SCOPES
            hot_vec = &s->fns.slot_body_scopes_hot;
            cold_vec = &s->fns.slot_body_scopes_cold;
        }
        break;
    }
    default:
        return false;
    }

    if (out_hot)  *out_hot  = (QuerySlotHot *)paged_get(hot_vec, row);
    if (out_cold) *out_cold = (QuerySlotCold *)paged_get(cold_vec, row);
    return true;
}

QuerySlotHot *db_engine_locate_slot(db_query_ctx *ctx, QueryKind kind,
                                    uint64_t key) {
    QuerySlotHot *hot = NULL;
    QuerySlotCold *cold = NULL;
    if (!db_engine_route_slot(ctx, kind, key, &hot, &cold)) return NULL;
    return hot;
}

QuerySlotCold *db_engine_locate_slot_cold(db_query_ctx *ctx, QueryKind kind,
                                          uint64_t key) {
    QuerySlotHot *hot = NULL;
    QuerySlotCold *cold = NULL;
    if (!db_engine_route_slot(ctx, kind, key, &hot, &cold)) return NULL;
    return cold;
}

// ============================================================================
// Running-slot tracker
// ============================================================================

void db_engine_track_running(db_query_ctx *ctx, QueryKind kind, uint64_t key) {
    struct db *s = db_(ctx);
    QueryRunningRef ref = {.kind = kind, .key = key};
    vec_push(&s->running_slots, &ref);
}

void db_engine_sweep_running(db_query_ctx *ctx) {
    struct db *s = db_(ctx);
    for (size_t i = 0; i < s->running_slots.count; i++) {
        QueryRunningRef *ref = (QueryRunningRef *)vec_get(&s->running_slots, i);
        QuerySlotHot *slot = db_engine_locate_slot(ctx, ref->kind, ref->key);
        if (slot && slot->state == QUERY_RUNNING) {
            slot->state = QUERY_EMPTY;
            slot->fingerprint = FINGERPRINT_NONE;
            // H16: cancel-bail in succeed/fail leaves deps assigned but
            // the slot in RUNNING. Free them as part of the reset, same
            // as reclaim_slot — otherwise the malloc-backed data buffers
            // leak on every canceled request.
            if (slot->deps)      vec_free(slot->deps);
            if (slot->dep_index) hashmap_free(slot->dep_index);
            slot->deps = NULL;
            slot->dep_index = NULL;
        }
    }
    vec_clear(&s->running_slots);
}

// ============================================================================
// State machine
// ============================================================================

QueryBeginResult db_query_begin(db_query_ctx *ctx, QueryKind kind,
                                uint64_t key) {
    if (db_check_cancel(ctx)) return QUERY_BEGIN_CANCELED;

    struct db *s = db_(ctx);
    s->query_stats[(int)kind].begin++;

    QuerySlotHot  *slot = NULL;
    QuerySlotCold *cold = NULL;
    bool ok = db_engine_route_slot(ctx, kind, key, &slot, &cold);
    assert(ok && slot && cold && "db_query_begin: db_engine_route_slot returned NULL — slot kind not wired");
    (void)ok;

    switch (slot->state) {
    case QUERY_DONE:
    case QUERY_ERROR: {
        QueryState prev = slot->state;
        QueryBeginResult cached_result =
            (prev == QUERY_DONE) ? QUERY_BEGIN_CACHED : QUERY_BEGIN_ERROR;

        // Push frame for verify; nested dep pulls record onto this frame.
        slot->state = QUERY_RUNNING;
        query_stack_push(ctx, kind, key, slot->deps, slot->dep_index);

        if (db_engine_verify(ctx, slot)) {
            slot->state = prev;
            slot->verified_rev = db_effective_revision(ctx);
            record_dep_on_parent(ctx, kind, key, slot->fingerprint,
                                 slot->durability, 1);
            query_stack_pop(ctx);
            s->query_stats[(int)kind].cached_hit++;
            return cached_result;
        }

        // Verify failed → recompute. Clear deps + diags; frame stays.
        QueryFrame *top = (QueryFrame *)vec_get(&s->query_stack,
                                                 s->query_stack.count - 1);
        if (top->deps) vec_clear(top->deps);
        if (top->dep_index) hashmap_clear(top->dep_index);
        slot->fingerprint = FINGERPRINT_NONE;
        db_diags_clear(s, kind, key);
        db_engine_track_running(ctx, kind, key);
        return QUERY_BEGIN_COMPUTE;
    }
    case QUERY_RUNNING:
        s->query_stats[(int)kind].cycle++;
        return QUERY_BEGIN_CYCLE;
    case QUERY_EMPTY:
        slot->state = QUERY_RUNNING;
        cold->routing_key = key;  // Stamp once; survives reclamation.
        query_stack_push(ctx, kind, key, slot->deps, slot->dep_index);
        db_engine_track_running(ctx, kind, key);
        return QUERY_BEGIN_COMPUTE;
    }
    return QUERY_BEGIN_CYCLE; // unreachable
}

void db_query_succeed(db_query_ctx *ctx, QueryKind kind, uint64_t key,
                      Fingerprint fp) {
    struct db *s = db_(ctx);
    assert(!db_query_kind_is_input(kind) &&
           "db_query_succeed: INPUT kinds are set via db_input_set, never "
           "computed");
    QuerySlotHot  *slot = db_engine_locate_slot(ctx, kind, key);
    QuerySlotCold *cold = db_engine_locate_slot_cold(ctx, kind, key);
    assert(slot && cold && "db_query_succeed: slot not located");

    QueryFrame *top = (QueryFrame *)vec_get(&s->query_stack,
                                             s->query_stack.count - 1);
    assert(top->kind == kind && top->key == key &&
           "db_query_succeed: top of stack doesn't match (kind, key)");

    // Transfer frame's deps to slot regardless of cancellation.
    // (Slot owned them via inheritance; lazy-allocated additions or
    // recompute-cleared-and-refilled state all live in top->deps/dep_index.)
    // On cancellation, sweep frees them when resetting RUNNING → EMPTY.
    slot->deps      = top->deps;
    slot->dep_index = top->dep_index;

    // H16: Cancellation observed mid-compute. Sub-queries returned
    // canceled-sentinel values that may have polluted our local computation
    // and the result column. We MUST NOT cache this result. Leave the slot
    // in QUERY_RUNNING so db_engine_sweep_running resets it to EMPTY at
    // request_end. Consumers see state != DONE and won't read the result
    // column. Next request: re-COMPUTE from scratch with clean inputs.
    if (db_check_cancel(ctx)) {
        query_stack_pop(ctx);
        return;
    }

    uint64_t cur = db_current_revision(ctx);
    slot->state         = QUERY_DONE;
    slot->fingerprint   = fp;
    slot->verified_rev  = cur;
    slot->durability    = top->dur_set ? top->min_input_dur : DUR_LOW;
    cold->computed_rev  = cur;

    record_dep_on_parent(ctx, kind, key, fp, slot->durability, 1);
    query_stack_pop(ctx);
    s->query_stats[(int)kind].compute++;
}

void db_query_fail(db_query_ctx *ctx, QueryKind kind, uint64_t key) {
    struct db *s = db_(ctx);
    QuerySlotHot  *slot = db_engine_locate_slot(ctx, kind, key);
    QuerySlotCold *cold = db_engine_locate_slot_cold(ctx, kind, key);
    assert(slot && cold && "db_query_fail: slot not located");

    QueryFrame *top = (QueryFrame *)vec_get(&s->query_stack,
                                             s->query_stack.count - 1);
    assert(top->kind == kind && top->key == key &&
           "db_query_fail: top of stack doesn't match (kind, key)");

    slot->deps      = top->deps;
    slot->dep_index = top->dep_index;

    // H16: Same cancel-bail logic as db_query_succeed — never cache a
    // failure that was induced by cancellation. Leave slot RUNNING for
    // sweep cleanup.
    if (db_check_cancel(ctx)) {
        query_stack_pop(ctx);
        return;
    }

    uint64_t cur = db_current_revision(ctx);
    slot->state        = QUERY_ERROR;
    slot->fingerprint  = FINGERPRINT_NONE;
    slot->verified_rev = cur;
    slot->durability   = top->dur_set ? top->min_input_dur : DUR_LOW;
    cold->computed_rev = cur;

    query_stack_pop(ctx);
    s->query_stats[(int)kind].error++;
}

void db_input_set(db_query_ctx *ctx, QueryKind kind, uint64_t key,
                  Fingerprint fp, Durability dur) {
    assert(db_query_kind_is_input(kind) &&
           "db_input_set: kind is not an INPUT kind — derived slots are set "
           "via db_query_succeed, not db_input_set");
    QuerySlotHot  *slot = db_engine_locate_slot(ctx, kind, key);
    QuerySlotCold *cold = db_engine_locate_slot_cold(ctx, kind, key);
    assert(slot && cold &&
           "db_input_set: slot not located (grow the input slot row first)");
    assert(dur < DUR_COUNT && "db_input_set: invalid durability");

    uint64_t cur = db_current_revision(ctx);
    slot->state        = QUERY_DONE;
    slot->fingerprint  = fp;
    slot->durability   = dur;
    slot->verified_rev = cur;
    cold->computed_rev = cur;
    cold->routing_key  = key;
}

void db_query_slot_alloc(db_query_ctx *ctx, QueryKind kind, uint64_t key) {
    // For HashMap-routed kinds, allocate a row if missing. For Vec-indexed
    // kinds, the caller's setter (db_create_file, db_create_namespace, …)
    // already grew the column.
    struct db *s = db_(ctx);
    switch (kind) {
    case QUERY_TOP_LEVEL_ENTRY: {
        if (hashmap_get(&s->top_level_entry_cache, key)) return;
        uint32_t row = (uint32_t)paged_count(&s->top_level_entry.slots_hot);
        paged_push_zero(&s->top_level_entry.results);
        paged_push_zero(&s->top_level_entry.keys);
        paged_push_zero(&s->top_level_entry.slots_hot);
        paged_push_zero(&s->top_level_entry.slots_cold);
        ((QuerySlotCold *)paged_get(&s->top_level_entry.slots_cold, row))->routing_key = key;
        hashmap_put_or_die(&s->top_level_entry_cache, key,
                           (void *)(uintptr_t)row,
                           "engine: top_level_entry slot alloc");
        return;
    }
    case QUERY_DEF_IDENTITY: {
        if (hashmap_get(&s->def_by_identity, key)) return;
        uint32_t row = (uint32_t)paged_count(&s->def_identity.slots_hot);
        paged_push_zero(&s->def_identity.results);
        paged_push_zero(&s->def_identity.slots_hot);
        paged_push_zero(&s->def_identity.slots_cold);
        ((QuerySlotCold *)paged_get(&s->def_identity.slots_cold, row))->routing_key = key;
        hashmap_put_or_die(&s->def_by_identity, key,
                           (void *)(uintptr_t)row, "engine: def_identity slot alloc");
        return;
    }
    case QUERY_RESOLVE_REF: {
        if (hashmap_get(&s->resolve_ref_cache, key)) return;
        uint32_t row = (uint32_t)paged_count(&s->resolve_ref.slots_hot);
        paged_push_zero(&s->resolve_ref.results);
        paged_push_zero(&s->resolve_ref.slots_hot);
        paged_push_zero(&s->resolve_ref.slots_cold);
        ((QuerySlotCold *)paged_get(&s->resolve_ref.slots_cold, row))->routing_key = key;
        hashmap_put_or_die(&s->resolve_ref_cache, key,
                           (void *)(uintptr_t)row, "engine: resolve_ref slot alloc");
        return;
    }
    default:
        // Vec-indexed kinds: column already sized at db_create_* time.
        return;
    }
}

// ============================================================================
// Public slot accessors
// ============================================================================

bool db_slot_is_live(db_query_ctx *ctx, QueryKind kind, uint64_t key) {
    QuerySlotHot *slot = db_engine_locate_slot(ctx, kind, key);
    if (!slot) return false;
    return slot->state == QUERY_DONE &&
           slot->verified_rev == db_current_revision(ctx);
}

QueryState db_slot_state(db_query_ctx *ctx, QueryKind kind, uint64_t key) {
    QuerySlotHot *slot = db_engine_locate_slot(ctx, kind, key);
    return slot ? slot->state : QUERY_EMPTY;
}

Fingerprint db_slot_fingerprint(db_query_ctx *ctx, QueryKind kind,
                                uint64_t key) {
    QuerySlotHot *slot = db_engine_locate_slot(ctx, kind, key);
    return slot ? slot->fingerprint : FINGERPRINT_NONE;
}

uint64_t db_slot_verified_rev(db_query_ctx *ctx, QueryKind kind, uint64_t key) {
    QuerySlotHot *slot = db_engine_locate_slot(ctx, kind, key);
    return slot ? slot->verified_rev : 0;
}

uint64_t db_slot_computed_rev(db_query_ctx *ctx, QueryKind kind, uint64_t key) {
    QuerySlotCold *cold = db_engine_locate_slot_cold(ctx, kind, key);
    return cold ? cold->computed_rev : 0;
}

// ============================================================================
// Stats
// ============================================================================

QueryStats db_query_stats(db_query_ctx *ctx, QueryKind kind) {
    return db_(ctx)->query_stats[(int)kind];
}

void db_query_stats_reset(db_query_ctx *ctx) {
    struct db *s = db_(ctx);
    memset(s->query_stats, 0, sizeof(s->query_stats));
}

// ============================================================================
// Frame accessors
// ============================================================================

QueryFrame *db_query_stack_top(db_query_ctx *ctx) {
    struct db *s = db_(ctx);
    if (s->query_stack.count == 0) return NULL;
    return (QueryFrame *)vec_get(&s->query_stack, s->query_stack.count - 1);
}

QueryKind db_frame_kind(const QueryFrame *f) { return f->kind; }
uint64_t  db_frame_key(const QueryFrame *f)  { return f->key; }

// ============================================================================
// Request scoping
// ============================================================================

// Atomically replace the request-revision bits of rev_control without
// stomping current_revision or invalidation.
static void rev_set_request(struct db *s, uint64_t request_bits) {
    uint64_t old = atomic_load(&s->rev_control);
    uint64_t new_val;
    do {
        new_val = (old & ~REV_REQUEST_MASK) | (request_bits & REV_REQUEST_MASK);
    } while (!atomic_compare_exchange_weak(&s->rev_control, &old, new_val));
}

void db_request_begin(db_query_ctx *ctx, uint64_t revision) {
    struct db *s = db_(ctx);
    assert(revision != 0 && "revision 0 is unpinned sentinel");
    assert(s->query_stack.count == 0 && "request begin while query on stack");
    // Pinning to a future revision is meaningless (no inputs at that rev
    // yet); pinning past current_rev would observe inputs that haven't
    // been published. Either is a caller bug.
    {
        uint64_t cur =
            (atomic_load(&s->rev_control) & REV_CURRENT_MASK) >> REV_CURRENT_SHIFT;
        assert(revision <= cur && "db_request_begin: revision exceeds current");
        // Suppress unused warning when assertions are disabled.
        (void)cur;
    }

    rev_set_request(s, revision);
    atomic_store(&s->cancel_requested, false);
    arena_reset(&s->request_arena);
    vec_clear(&s->running_slots);
}

void db_request_end(db_query_ctx *ctx) {
    struct db *s = db_(ctx);
    assert(s->query_stack.count == 0 && "request end while query on stack");
    // Detect end-without-begin: request bits should be non-zero from a
    // prior db_request_begin. Catches the caller-bug of unbalanced
    // begin/end pairs.
    assert((atomic_load(&s->rev_control) & REV_REQUEST_MASK) != 0 &&
           "db_request_end: no request open (begin/end mismatch)");

    db_engine_sweep_running(ctx);
    rev_set_request(s, 0);
    arena_reset(&s->request_arena);
    db_engine_compact(ctx);
}

void db_request_cancel(db_query_ctx *ctx) {
    atomic_store(&db_(ctx)->cancel_requested, true);
}

bool db_check_cancel(db_query_ctx *ctx) {
    return atomic_load(&db_(ctx)->cancel_requested);
}

// ============================================================================
// Inputs + revisions
// ============================================================================

uint64_t db_current_revision(db_query_ctx *ctx) {
    return (atomic_load(&db_(ctx)->rev_control) & REV_CURRENT_MASK) >>
           REV_CURRENT_SHIFT;
}

uint64_t db_effective_revision(db_query_ctx *ctx) {
    uint64_t c = atomic_load(&db_(ctx)->rev_control);
    uint64_t r = c & REV_REQUEST_MASK;
    return r != 0 ? r : ((c & REV_CURRENT_MASK) >> REV_CURRENT_SHIFT);
}

uint64_t db_input_changed(db_query_ctx *ctx, Durability dur) {
    struct db *s = db_(ctx);
    assert(dur < DUR_COUNT && "db_input_changed: durability out of range");
    // Input changes during an open request violate the snapshot invariant
    // (pinned effective_revision assumes inputs are stable).
    assert((atomic_load(&s->rev_control) & REV_REQUEST_MASK) == 0 &&
           "db_input_changed called while a request is open");

    // Bump current_revision atomically.
    uint64_t old = atomic_load(&s->rev_control);
    uint64_t new_cur;
    uint64_t new_val;
    do {
        uint64_t cur = (old & REV_CURRENT_MASK) >> REV_CURRENT_SHIFT;
        new_cur = cur + 1;
        new_val = (old & ~REV_CURRENT_MASK) |
                  ((new_cur << REV_CURRENT_SHIFT) & REV_CURRENT_MASK);
    } while (!atomic_compare_exchange_weak(&s->rev_control, &old, new_val));

    // Stamp dur_last_changed for all tiers at or below `dur`. A query
    // whose MIN-tier <= dur must verify its deps; a query whose MIN-tier
    // > dur skips the dep walk.
    for (Durability d = DUR_LOW; d <= dur; d++) {
        atomic_store(&s->dur_last_changed[d], new_cur);
    }
    return new_cur;
}

void db_query_note_input_durability(db_query_ctx *ctx, Durability dur) {
    struct db *s = db_(ctx);
    assert(dur < DUR_COUNT && "db_query_note_input_durability: durability out of range");
    if (s->query_stack.count == 0) return;
    QueryFrame *top = (QueryFrame *)vec_get(&s->query_stack,
                                             s->query_stack.count - 1);
    if (!top->dur_set || dur < top->min_input_dur) {
        top->min_input_dur = dur;
    }
    top->dur_set = true;
}

// ============================================================================
// Engine init / free
// ============================================================================

void db_engine_init(db_query_ctx *ctx) {
    struct db *s = db_(ctx);
    // Schema lifecycle (every PagedVec / Vec column, the arena-backed
    // query_stack) lives in db_ids_init. Engine init owns only
    // engine-state: stats counters, running_slots scratch, cancel
    // token, and the top_level_entry routing HashMap.
    memset(s->query_stats, 0, sizeof(s->query_stats));
    vec_init(&s->running_slots, sizeof(QueryRunningRef));
    atomic_store(&s->cancel_requested, false);
    hashmap_init(&s->top_level_entry_cache);
}

void db_engine_free(db_query_ctx *ctx) {
    struct db *s = db_(ctx);

    // H22: walk every slot column, free per-slot deps/dep_index buffers
    // and per-kind result-struct embedded HashMaps. Must run BEFORE
    // db_ids_free drops the columns those slots live in — otherwise
    // the rows' heap is unreachable when the columns disappear.
    db_engine_deep_free(ctx);

    // running_slots is malloc-backed engine state. query_stack lives
    // in s->arena and is reclaimed when the arena is freed in db_free,
    // so no explicit free here.
    vec_free(&s->running_slots);

    hashmap_free(&s->top_level_entry_cache);
}
