#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include "../src/db/db.h"
#include "../src/db/query/query.h"
#include "../src/db/query/invalidate.h"
#include "../src/db/request/request.h"

/* ---------------------------------------------------------------------- */
/* Full db lifecycle. Every test now exercises StringPool, InternPool,    */
/* HashMap caches, and pre-interned hot names — not just the query bits.  */
/* ---------------------------------------------------------------------- */

static struct db g_db;

static void setup(void)    { db_init(&g_db); }
static void teardown(void) { db_free(&g_db); }

// Allocate a fresh DefId. db_alloc_def pushes zero rows to every defs
// column, so the slot at &defs.slots_type[def.idx] has state=QUERY_EMPTY
// and kind=0 (which happens to be QUERY_TYPE_OF_DECL — the slot column's
// kind by convention). No explicit slot_init needed for tests.
static DefId alloc_def(void) {
    return db_alloc_def(&g_db);
}

// Borrow a pointer to the type slot for a DefId. ONLY safe to use before
// any subsequent db_alloc_def — Vec realloc invalidates this borrow.
// Tests that need stable access use db_locate_slot.
static QuerySlot *slot_for_def(DefId d) {
    return (QuerySlot *)vec_get(&g_db.defs.slots_type, d.idx);
}

/* ---------------------------------------------------------------------- */
/* Tests                                                                  */
/* ---------------------------------------------------------------------- */

static void test_slot_init_state(void) {
    setup();
    DefId d = alloc_def();
    QuerySlot *slot = slot_for_def(d);
    assert(slot->state == QUERY_EMPTY);
    assert(slot->fingerprint == FINGERPRINT_NONE);
    assert(slot->computed_rev == 0);
    assert(slot->verified_rev == 0);
    assert(slot->changed_rev == 0);
    assert(slot->deps == NULL);
    assert(slot->diags == NULL);
    teardown();
}

static void test_begin_compute_on_empty(void) {
    setup();
    DefId d = alloc_def();
    QueryBeginResult r = db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &d);
    assert(r == QUERY_BEGIN_COMPUTE);
    assert(slot_for_def(d)->state == QUERY_RUNNING);
    assert(g_db.query_stack.count == 1);
    QueryFrame *top = db_query_stack_top(&g_db);
    assert(top->kind == QUERY_TYPE_OF_DECL);
    assert(top->key == &d);
    teardown();
}

static void test_succeed_marks_done_and_stores_fp(void) {
    setup();
    DefId d = alloc_def();
    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &d);
    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &d, 0xABCDEFu);
    QuerySlot *slot = slot_for_def(d);
    assert(slot->state == QUERY_DONE);
    assert(slot->fingerprint == 0xABCDEFu);
    assert(slot->computed_rev == 1);
    assert(slot->verified_rev == 1);
    assert(g_db.query_stack.count == 0);
    teardown();
}

static void test_cached_on_done_at_same_rev(void) {
    setup();
    DefId d = alloc_def();
    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &d);
    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &d, 42);

    QueryBeginResult r = db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &d);
    assert(r == QUERY_BEGIN_CACHED);
    assert(g_db.query_stack.count == 0); // No frame pushed on cached path.
    teardown();
}

static void test_cycle_on_running(void) {
    setup();
    DefId d = alloc_def();
    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &d);
    QueryBeginResult r = db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &d);
    assert(r == QUERY_BEGIN_CYCLE);
    teardown();
}

static void test_cancel_propagates(void) {
    setup();
    DefId d = alloc_def();
    db_request_cancel(&g_db);
    QueryBeginResult r = db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &d);
    assert(r == QUERY_BEGIN_CANCELED);
    teardown();
}

static void test_fail_marks_error(void) {
    setup();
    DefId d = alloc_def();
    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &d);
    db_query_fail(&g_db, QUERY_TYPE_OF_DECL, &d);
    QuerySlot *slot = slot_for_def(d);
    assert(slot->state == QUERY_ERROR);
    assert(slot->verified_rev == 1);
    assert(g_db.query_stack.count == 0);
    teardown();
}

// A child query that returns CACHED inside a parent's body should
// record a dep on the parent's frame.
static void test_dep_recorded_on_parent(void) {
    setup();
    DefId b = alloc_def();
    DefId a = alloc_def();

    // Prime B as DONE so the inner begin returns CACHED.
    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &b);
    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &b, 0xB00B);

    // Parent A begins; inside its body it asks for B (cached).
    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &a);
    QueryBeginResult inner = db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &b);
    assert(inner == QUERY_BEGIN_CACHED);

    // A's frame should now hold a dep for B.
    QueryFrame *top = db_query_stack_top(&g_db);
    assert(top->deps != NULL);
    assert(top->deps->count == 1);
    QueryDep *dep = (QueryDep *)vec_get(top->deps, 0);
    assert(dep->kind == QUERY_TYPE_OF_DECL);
    assert(dep->key == &b);
    assert(dep->dep_fp == 0xB00B);

    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &a, 0xAAA);

    // Slot A should now own the same dep list.
    QuerySlot *slot_a = slot_for_def(a);
    assert(slot_a->deps != NULL);
    assert(slot_a->deps->count == 1);
    teardown();
}

// Recompute path: change a dep's fingerprint between revisions and
// verify the parent recomputes (begin returns COMPUTE, not CACHED).
static void test_revalidate_recompute_on_dep_fp_mismatch(void) {
    setup();
    DefId b = alloc_def();
    DefId a = alloc_def();

    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &b);
    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &b, 100);

    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &a);
    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &b); // cached → records dep
    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &a, 200);

    // Simulate edit: bump revision and change B's fingerprint.
    g_db.current_revision = 2;
    slot_for_def(b)->fingerprint = 999;
    slot_for_def(b)->verified_rev = 2;    // pretend B was re-verified
    slot_for_def(b)->computed_rev = 2;

    // A should recompute now.
    QueryBeginResult r = db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &a);
    assert(r == QUERY_BEGIN_COMPUTE);
    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &a, 201);
    teardown();
}

// Stable deps: re-running A at a fresh revision when B's fingerprint
// hasn't changed should hit CACHED (early cutoff via fingerprint match).
static void test_revalidate_skip_when_deps_stable(void) {
    setup();
    DefId b = alloc_def();
    DefId a = alloc_def();

    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &b);
    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &b, 100);

    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &a);
    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &b);
    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &a, 200);

    g_db.current_revision = 2;
    // B was not modified — its fingerprint stays 100, but verified_rev is
    // still 1. db_revalidate should walk, see fp matches, and skip.

    QueryBeginResult r = db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &a);
    assert(r == QUERY_BEGIN_CACHED);
    teardown();
}

// Untracked reads force RECOMPUTE on revalidate regardless of deps.
static void test_revalidate_recompute_when_untracked(void) {
    setup();
    DefId d = alloc_def();

    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &d);
    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &d, 42);
    slot_for_def(d)->has_untracked_read = true;

    g_db.current_revision = 2;
    QueryBeginResult r = db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &d);
    assert(r == QUERY_BEGIN_COMPUTE);
    teardown();
}

// ERROR slots revalidate like DONE — a previously-failed query must be
// retried after an edit that might have fixed the cause.
static void test_error_state_revalidates(void) {
    setup();
    DefId d = alloc_def();

    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &d);
    db_query_fail(&g_db, QUERY_TYPE_OF_DECL, &d);
    assert(slot_for_def(d)->state == QUERY_ERROR);

    // No deps + no untracked + new revision → revalidate walks empty
    // deps, finds nothing wrong, marks verified at new rev, returns
    // SKIP_RECOMPUTE. The ERROR state then surfaces as QUERY_BEGIN_ERROR
    // to the caller (not RECOMPUTE).
    g_db.current_revision = 2;
    QueryBeginResult r = db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &d);
    assert(r == QUERY_BEGIN_ERROR);

    // But if there were a dep whose fp changed, ERROR would recompute.
    // Force-recompute by setting has_untracked_read.
    slot_for_def(d)->has_untracked_read = true;
    slot_for_def(d)->verified_rev = 1; // un-verify so revalidate walks again
    g_db.current_revision = 3;
    r = db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &d);
    assert(r == QUERY_BEGIN_COMPUTE);
    teardown();
}

// Bug-1 regression test: caller never holds a slot pointer; the engine
// re-resolves at every boundary. To prove the fix, allocate enough new
// DefIds DURING a parent's body to force defs.slots_type to realloc its
// malloc buffer. The parent's succeed call must still write to the
// correct (new) slot location, not a dangling one. Run under ASan/UBSan
// for the strongest signal.
static void test_pointer_stability_under_alloc(void) {
    setup();
    DefId parent = alloc_def();
    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &parent);

    // Allocate enough defs to force one or more reallocs of
    // defs.slots_type. The default malloc-Vec doubles, so any sustained
    // growth path will eventually realloc; this gives us many tries.
    for (int i = 0; i < 64; i++) {
        (void)alloc_def();
    }

    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &parent, 0xDEADBEEF);

    // After the dust settles, the parent's slot at its DefId must
    // reflect the succeed call. If we'd written through a dangling
    // pointer this would be UB; under ASan, the test would have crashed
    // before this line.
    QuerySlot *p = slot_for_def(parent);
    assert(p->state == QUERY_DONE);
    assert(p->fingerprint == 0xDEADBEEF);
    teardown();
}

// Bug-2 regression test: the deps Vec object pointer should be reused
// across recomputes (zero arena leak per recompute cycle).
static void test_deps_vec_reused_across_recomputes(void) {
    setup();
    DefId b = alloc_def();
    DefId a = alloc_def();

    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &b);
    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &b, 100);

    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &a);
    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &b); // records dep
    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &a, 200);

    Vec *deps_first = slot_for_def(a)->deps;
    assert(deps_first != NULL);

    // Force recompute by mutating B's fingerprint, then re-run A.
    g_db.current_revision = 2;
    slot_for_def(b)->fingerprint = 999;
    slot_for_def(b)->verified_rev = 2;
    slot_for_def(b)->computed_rev = 2;

    QueryBeginResult r = db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &a);
    assert(r == QUERY_BEGIN_COMPUTE);
    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &b); // re-records dep
    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &a, 201);

    Vec *deps_second = slot_for_def(a)->deps;
    assert(deps_second == deps_first &&
           "deps Vec object must be the same pointer across recomputes");

    // And the malloc buffer should be reused too — count is back to 1
    // (one dep on B), not appended to the prior cycle's deps.
    assert(deps_second->count == 1);
    teardown();
}

/* ---------------------------------------------------------------------- */
/* Lifecycle smoke tests                                                  */
/* ---------------------------------------------------------------------- */

static void test_lifecycle_pre_interned_names(void) {
    setup();
    assert(g_db.names.sizeOf.idx != 0);
    assert(g_db.names.alignOf.idx != 0);
    assert(g_db.names.TypeOf.idx != 0);
    assert(g_db.names.intCast.idx != 0);
    assert(g_db.names.typeName.idx != 0);
    // All five must be distinct.
    assert(g_db.names.sizeOf.idx   != g_db.names.alignOf.idx);
    assert(g_db.names.sizeOf.idx   != g_db.names.TypeOf.idx);
    assert(g_db.names.alignOf.idx  != g_db.names.TypeOf.idx);
    assert(g_db.names.intCast.idx  != g_db.names.typeName.idx);
    teardown();
}

static void test_lifecycle_scalar_defaults(void) {
    setup();
    assert(g_db.current_revision == 1);
    assert(g_db.request_revision == 0);
    assert(g_db.invalidation_enabled == true);
    assert(g_db.comptime_depth_limit == 256);
    teardown();
}

// db_init → db_free → db_init must produce a clean working db both
// times. Catches: state not fully zeroed on free, static pointers
// lingering, double-free on re-init, etc.
static void test_lifecycle_init_free_init_idempotent(void) {
    db_init(&g_db);
    DefId d1 = db_alloc_def(&g_db);
    (void)d1;
    db_free(&g_db);

    db_init(&g_db);
    DefId d2 = db_alloc_def(&g_db);
    // After re-init, DefIds restart from 1 (slot 0 is NONE sentinel).
    assert(d2.idx == 1);
    assert(g_db.names.sizeOf.idx != 0);
    assert(g_db.current_revision == 1);
    db_free(&g_db);

    memset(&g_db, 0, sizeof(g_db));
}

/* ---------------------------------------------------------------------- */

int main(void) {
    test_slot_init_state();
    test_begin_compute_on_empty();
    test_succeed_marks_done_and_stores_fp();
    test_cached_on_done_at_same_rev();
    test_cycle_on_running();
    test_cancel_propagates();
    test_fail_marks_error();
    test_dep_recorded_on_parent();
    test_revalidate_recompute_on_dep_fp_mismatch();
    test_revalidate_skip_when_deps_stable();
    test_revalidate_recompute_when_untracked();
    test_error_state_revalidates();
    test_pointer_stability_under_alloc();
    test_deps_vec_reused_across_recomputes();

    test_lifecycle_pre_interned_names();
    test_lifecycle_scalar_defaults();
    test_lifecycle_init_free_init_idempotent();

    printf("query + lifecycle tests: 17/17 passed\n");
    return 0;
}
