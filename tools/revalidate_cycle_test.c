// Base-layer gate — db_revalidate must not stack-overflow on a cyclic
// dependency graph. The parse-only graph is a DAG, so this can't be
// hit yet; sema's queries are mutually recursive (type <-> signature
// <-> const-eval), so the first cyclic graph it revalidates would
// recurse unboundedly without the in-progress guard.
//
// We build that exact shape directly: two DefId-keyed slots whose deps
// point at each other, both DONE with a stale verified_rev, durability
// fast-path forced to decline. db_revalidate(A) must return RECOMPUTE
// (a cycle can't be proven unchanged) and unwind cleanly — not crash —
// and must leave no `revalidating` mark behind.

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/query/invalidate.h"
#include "../src/db/query/query.h"

#include <stdio.h>

// Allocate a deps Vec exactly as the engine does: the Vec struct in
// db.arena (reclaimed with the arena by db_free), the backing buffer
// malloc-owned (freed by db_free's per-slot cleanup visitor). No
// manual teardown — mirroring production lifetime precisely.
static Vec *one_dep(struct db *db, QueryKind k, const void *key,
                    Fingerprint fp) {
  Vec *v = (Vec *)arena_alloc(&db->arena, sizeof(Vec));
  vec_init(v, sizeof(QueryDep));
  QueryDep d = {.kind = k, .key = key, .dep_fp = fp};
  vec_push(v, &d);
  return v;
}

int main(void) {
  struct db db;
  db_init(&db);

  // Allocate ALL defs up front, THEN locate slots: db_alloc_def grows
  // the slots_type Vec, which can realloc and invalidate previously
  // returned slot pointers. DefId key variables are stack-stable.
  DefId dA = db_alloc_def(&db);
  DefId dB = db_alloc_def(&db);
  DefId dC = db_alloc_def(&db);

  QuerySlot *A = db_locate_slot(&db, QUERY_TYPE_OF_DECL, &dA);
  QuerySlot *B = db_locate_slot(&db, QUERY_TYPE_OF_DECL, &dB);

  int ok = 1;
  if (!A || !B) {
    fprintf(stderr, "FAIL: slot lookup returned NULL\n");
    db_free(&db);
    printf("FAIL revalidate_cycle\n");
    return 1;
  }

  // A <-> B mutual dependency, both already computed at an old revision.
  A->state = QUERY_DONE;
  A->fingerprint = 0x1111;
  A->verified_rev = 1;
  A->durability = DUR_LOW;
  A->has_untracked_read = false;
  A->deps = one_dep(&db, QUERY_TYPE_OF_DECL, &dB, 0x2222);

  B->state = QUERY_DONE;
  B->fingerprint = 0x2222;
  B->verified_rev = 1;
  B->durability = DUR_LOW;
  B->has_untracked_read = false;
  B->deps = one_dep(&db, QUERY_TYPE_OF_DECL, &dA, 0x1111);

  // Advance the revision so verified_rev (1) != effective, and bump
  // the LOW tier so the durability fast-path declines and the walk
  // actually descends into the cycle.
  db_input_changed(&db, DUR_LOW);

  // The moment of truth: without the guard this never returns.
  RevalidateResult r = db_revalidate(&db, A);

  if (r != DB_REVALIDATE_RECOMPUTE) {
    fprintf(stderr, "FAIL: cyclic revalidate returned %d, want RECOMPUTE\n", r);
    ok = 0;
  }
  if (A->revalidating || B->revalidating) {
    fprintf(stderr, "FAIL: revalidating mark leaked (A=%d B=%d)\n",
            A->revalidating, B->revalidating);
    ok = 0;
  }

  // Acyclic control: a depless DONE slot still revalidates normally.
  QuerySlot *C = db_locate_slot(&db, QUERY_TYPE_OF_DECL, &dC);
  C->state = QUERY_DONE;
  C->verified_rev = db_effective_revision(&db); // already current
  if (db_revalidate(&db, C) != DB_REVALIDATE_SKIP_RECOMPUTE) {
    fprintf(stderr, "FAIL: acyclic depless slot did not SKIP\n");
    ok = 0;
  }

  db_free(&db); // owns teardown: per-slot deps-buffer free + arena

  if (ok) {
    printf("PASS revalidate_cycle: cyclic dep graph -> RECOMPUTE, no "
           "overflow, marks cleared\n");
    return 0;
  }
  printf("FAIL revalidate_cycle\n");
  return 1;
}
