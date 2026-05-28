// Pre-Phase-C debt D-HM — orphan reclamation removes routing-HashMap
// entries (two-pass) so the routing maps stay bounded across a long LSP
// session, instead of growing monotonically as DefIds/refs churn.
//
// Before D-HM, reclaim_hashmap_kind zeroed the orphan slot but left the
// routing entry pointing at the now-EMPTY row. The maps (resolve_ref_cache,
// decl_ast_cache, top_level_entry_cache, def_by_identity) never shrank.
//
// This test exercises the reclamation path directly against the keep-zone
// engine (no sema), under ASan, asserting:
//   1. After reclamation, a HashMap-routed kind's routing map is empty
//      (D-HM: entries removed, not just slots zeroed).
//   2. orphan_reclaimed telemetry matches the number of slots GC'd.
//   3. The reclaimed slots are no longer live.
//   4. Re-allocating the same keys after reclamation repopulates the map
//      with fresh rows — proving the two-pass removal didn't corrupt the
//      map (backward-shift cleanup leaves a consistent probe chain).
//   5. No use-after-free / leak (ASan gate on the two-pass removal +
//      db_free deep-free path).

#define ORE_ENGINE_PRIVATE
#include "../src/db/db.h"
#include "../src/db/query/engine.h"
#include "../src/db/query/engine_internal.h"

#include <assert.h>
#include <stdio.h>

#define N_REFS 64

// Build a RESOLVE_REF key the same way the engine does: (scope<<32)|name.idx.
static uint64_t resolve_ref_key(uint32_t scope, uint32_t name_idx) {
    return ((uint64_t)scope << 32) | (uint64_t)name_idx;
}

// Drive a RESOLVE_REF slot to DONE at the current revision via the normal
// query lifecycle (slot_alloc inserts the routing entry; begin EMPTY→COMPUTE;
// succeed records fp + verified_rev = current). This replaces the retired
// push-stamp primitive as the orphan-setup mechanism.
static void make_done(struct db *s, uint64_t key, uint64_t fpval) {
    db_query_slot_alloc(s, QUERY_RESOLVE_REF, key);
    QueryBeginResult r = db_query_begin(s, QUERY_RESOLVE_REF, key);
    assert(r == QUERY_BEGIN_COMPUTE && "fresh slot should COMPUTE");
    (void)r;
    db_query_succeed(s, QUERY_RESOLVE_REF, key, db_fp_u64(fpval));
}

int main(void) {
    struct db s;
    db_init(&s);

    // Bring N RESOLVE_REF slots to DONE at the current revision.
    uint64_t keys[N_REFS];
    for (uint32_t i = 0; i < N_REFS; i++) {
        keys[i] = resolve_ref_key(1, i + 1);
        make_done(&s, keys[i], i + 1);
    }

    assert(s.resolve_ref_cache.count == N_REFS &&
           "setup: all N routing entries present");
    for (uint32_t i = 0; i < N_REFS; i++)
        assert(db_slot_is_live(&s, QUERY_RESOLVE_REF, keys[i]) &&
               "setup: DONE slots live at current revision");

    // Advance the revision well past ENGINE_ORPHAN_THRESHOLD (8) WITHOUT
    // re-verifying. Every slot's verified_rev now falls behind → orphan.
    // db_input_changed must run outside a request.
    for (int r = 0; r < (int)ENGINE_ORPHAN_THRESHOLD + 4; r++)
        db_input_changed(&s, DUR_LOW);

    uint64_t cur = db_current_revision(&s);
    uint64_t threshold = cur - ENGINE_ORPHAN_THRESHOLD;

    QueryStats before = db_query_stats(&s, QUERY_RESOLVE_REF);
    uint64_t reclaimed = db_engine_reclaim_orphans(&s, threshold);
    QueryStats after = db_query_stats(&s, QUERY_RESOLVE_REF);

    // (1) D-HM: routing entries removed, not just slots zeroed.
    assert(s.resolve_ref_cache.count == 0 &&
           "D-HM: routing map empty after reclaiming all orphan slots");

    // (2) telemetry: every slot counted.
    assert(reclaimed == N_REFS && "all N slots reclaimed");
    assert(after.orphan_reclaimed - before.orphan_reclaimed == N_REFS &&
           "orphan_reclaimed telemetry matches reclaim count");

    // (3) reclaimed slots no longer live (route_slot misses → not live).
    for (uint32_t i = 0; i < N_REFS; i++)
        assert(!db_slot_is_live(&s, QUERY_RESOLVE_REF, keys[i]) &&
               "reclaimed slot is dead");

    // (4) re-allocate the same keys: the map must accept them again and
    //     route to fresh rows. This proves the backward-shift removal left
    //     a consistent map (no stale probe-chain breakage).
    for (uint32_t i = 0; i < N_REFS; i++)
        make_done(&s, keys[i], i + 100);
    assert(s.resolve_ref_cache.count == N_REFS &&
           "re-alloc after reclaim repopulates the routing map");
    for (uint32_t i = 0; i < N_REFS; i++)
        assert(db_slot_is_live(&s, QUERY_RESOLVE_REF, keys[i]) &&
               "re-computed slot live again");

    db_free(&s);  // ASan gate: deep-free + two-pass removal leave no leak/UAF.

    printf("PASS orphan_reclaim: D-HM removes routing entries (resolve_ref_cache "
           "%d->0->%d), telemetry + liveness + re-alloc consistent\n",
           N_REFS, N_REFS);
    return 0;
}
