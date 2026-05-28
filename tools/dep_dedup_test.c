// W1 regression — dep-dedup must be collision-safe.
//
// dep_index_key(kind, key) = (kind<<56) | (key & 0x00FF..FF) truncates the
// key's top 8 bits, so two DISTINCT (kind, key) deps whose keys differ
// only there collide in a frame's dep_index — even though they're
// distinct in the full-u64 routing map. The OLD code, on a hashmap hit,
// overwrote the existing dep's fp IN PLACE → the other dep was silently
// dropped (count stays 1) → verify misses its invalidation. The fix
// confirms (kind,key) on a hit and appends on collision, so BOTH survive.
//
// We force the collision cheaply: two RESOLVE_REF children with keys 5
// and (1<<56)|5 (same low 56 bits, differ in bit 56). A parent reads
// both; we assert the parent recorded TWO deps with both keys present.

#define ORE_ENGINE_PRIVATE
#include "../src/db/db.h"
#include "../src/db/query/engine.h"
#include "../src/db/query/engine_internal.h"
#include "../src/support/data_structure/vec.h"

#include <assert.h>
#include <stdio.h>

static void bring_done(struct db *s, uint64_t key) {
    db_query_slot_alloc(s, QUERY_RESOLVE_REF, key);
    QueryBeginResult r = db_query_begin(s, QUERY_RESOLVE_REF, key);
    assert(r == QUERY_BEGIN_COMPUTE && "fresh child should COMPUTE");
    (void)r;
    db_query_succeed(s, QUERY_RESOLVE_REF, key, db_fp_u64(key));
}

int main(void) {
    struct db s;
    db_init(&s);
    db_request_begin(&s, db_current_revision(&s));

    const uint64_t k1 = 5ULL;                 // child A
    const uint64_t k2 = (1ULL << 56) | 5ULL;  // child B — collides with A in dep_index_key
    const uint64_t kp = 99ULL;                // parent

    bring_done(&s, k1);
    bring_done(&s, k2);

    // Parent reads both children, recording a dep on each.
    db_query_slot_alloc(&s, QUERY_RESOLVE_REF, kp);
    assert(db_query_begin(&s, QUERY_RESOLVE_REF, kp) == QUERY_BEGIN_COMPUTE);
    assert(db_query_begin(&s, QUERY_RESOLVE_REF, k1) == QUERY_BEGIN_CACHED);
    assert(db_query_begin(&s, QUERY_RESOLVE_REF, k2) == QUERY_BEGIN_CACHED);
    db_query_succeed(&s, QUERY_RESOLVE_REF, kp, db_fp_u64(kp));

    QuerySlotHot *pslot = db_engine_locate_slot(&s, QUERY_RESOLVE_REF, kp);
    assert(pslot && pslot->deps && "parent recorded deps");
    assert(pslot->deps->count == 2 &&
           "both colliding-ikey deps recorded (no lost dep)");

    bool saw_k1 = false, saw_k2 = false;
    for (size_t i = 0; i < pslot->deps->count; i++) {
        QueryDep *d = (QueryDep *)vec_get(pslot->deps, i);
        if (d->kind == QUERY_RESOLVE_REF && d->key == k1) saw_k1 = true;
        if (d->kind == QUERY_RESOLVE_REF && d->key == k2) saw_k2 = true;
    }
    assert(saw_k1 && saw_k2 &&
           "both distinct dep keys survive the dep_index_key collision");

    db_request_end(&s);
    db_free(&s);
    printf("PASS dep_dedup: colliding-ikey deps both recorded (W1 collision-safe)\n");
    return 0;
}
