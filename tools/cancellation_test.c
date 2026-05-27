// Phase 0 P0 gap G1 — cancellation semantics.
//
// Engine contract being locked in:
//   1. db_query_begin returns QUERY_BEGIN_CANCELED when cancel_requested
//      is set.
//   2. If a query body returns early (without reaching succeed/fail),
//      the slot is left in QUERY_RUNNING.
//   3. db_request_end sweeps any QUERY_RUNNING slots back to QUERY_EMPTY.
//   4. The NEXT request after a cancelled one can compute the same query
//      freshly — the slot is recoverable, not poisoned.
//
// This is the load-bearing invariant for LSP cancellation: an in-flight
// hover/completion cancelled by the client must not leave the query
// engine in a state that blocks the next request.
//
// Note: this test does NOT cover the higher-level "DB_QUERY_GUARD honors
// CANCELED" behavior (audit Tier-0 B4). That's a separate concern —
// today bodies proceed into computation even when CANCELED is returned.
// This test exercises the engine primitives that the rewrite must
// preserve.

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/query/ast.h"
#include "../src/db/query/invalidate.h"
#include "../src/db/query/query.h"
#include "../src/db/request/request.h"

#include <stdio.h>
#include <string.h>

int main(void) {
  struct db db;
  db_init(&db);

  const char *path = "cancel.ore";
  const char *text = "A :: 1\n";
  SourceId src = db_create_source(&db, path, strlen(path), text, strlen(text));
  NamespaceId ns = db_create_namespace(&db);
  FileId fid = db_create_file(&db, src, ns);

  int ok = 1;

  // ---- Part A: db_query_begin returns CANCELED after db_request_cancel ----
  db_request_begin(&db, db_current_revision(&db));
  db_request_cancel(&db);

  if (!db_check_cancel(&db)) {
    fprintf(stderr, "FAIL: db_check_cancel returned false after request_cancel\n");
    ok = 0;
  }

  QueryBeginResult r = db_query_begin(&db, QUERY_FILE_AST, (uint64_t)fid.idx);
  if (r != QUERY_BEGIN_CANCELED) {
    fprintf(stderr, "FAIL: db_query_begin returned %d, want QUERY_BEGIN_CANCELED=%d\n",
            (int)r, (int)QUERY_BEGIN_CANCELED);
    ok = 0;
  }

  // CANCELED is returned BEFORE the slot transitions to RUNNING, so the
  // slot stays at EMPTY here. (Verified below.) The body is responsible
  // for honoring CANCELED — that's a future-fix; the engine's job is to
  // return CANCELED early. The slot lifecycle invariant is: a query that
  // returns CANCELED leaves the slot in whatever state it was in before,
  // and request_end's sweep handles any RUNNING leftovers from queries
  // that DID transition.
  QuerySlotHot *slot = db_locate_slot(&db, QUERY_FILE_AST, (uint64_t)fid.idx);
  if (!slot) {
    fprintf(stderr, "FAIL: slot not located after cancelled begin\n");
    ok = 0;
  } else if (slot->state != QUERY_EMPTY) {
    fprintf(stderr,
            "FAIL: after CANCELED return, slot state = %d, want QUERY_EMPTY=%d\n",
            slot->state, QUERY_EMPTY);
    ok = 0;
  }

  db_request_end(&db);

  // ---- Part B: request_end sweep resets RUNNING slots to EMPTY ----
  // We can't directly create a RUNNING leftover through the engine API
  // (CANCELED returns before RUNNING transition). Simulate by manually
  // pushing a slot to RUNNING and tracking it as a leftover, then call
  // request_end and verify the sweep cleaned it up.
  db_request_begin(&db, db_current_revision(&db));

  // Manually set the slot to RUNNING + register it as a "leftover" —
  // this is what would happen if a body began compute then bailed
  // before reaching succeed/fail.
  slot = db_locate_slot(&db, QUERY_FILE_AST, (uint64_t)fid.idx);
  slot->state = QUERY_RUNNING;
  QueryRunningRef leftover = {.kind = QUERY_FILE_AST, .key = (uint64_t)fid.idx};
  vec_push(&db.running_slots, &leftover);

  db_request_end(&db);

  slot = db_locate_slot(&db, QUERY_FILE_AST, (uint64_t)fid.idx);
  if (slot->state != QUERY_EMPTY) {
    fprintf(stderr,
            "FAIL: after request_end sweep, RUNNING leftover slot state = %d, "
            "want QUERY_EMPTY=%d\n",
            slot->state, QUERY_EMPTY);
    ok = 0;
  }
  if (slot->fingerprint != FINGERPRINT_NONE) {
    fprintf(stderr,
            "FAIL: after sweep, slot fingerprint = %llu, want FINGERPRINT_NONE=0\n",
            (unsigned long long)slot->fingerprint);
    ok = 0;
  }

  // ---- Part C: next request can compute the query freshly ----
  db_request_begin(&db, db_current_revision(&db));
  Fingerprint fp = db_query_file_ast(&db, fid);
  db_request_end(&db);

  if (fp == FINGERPRINT_NONE) {
    fprintf(stderr,
            "FAIL: after cancellation cleanup, file_ast produced FINGERPRINT_NONE\n");
    ok = 0;
  }

  slot = db_locate_slot(&db, QUERY_FILE_AST, (uint64_t)fid.idx);
  if (slot->state != QUERY_DONE) {
    fprintf(stderr,
            "FAIL: after fresh compute, slot state = %d, want QUERY_DONE=%d\n",
            slot->state, QUERY_DONE);
    ok = 0;
  }

  db_free(&db);

  if (ok) {
    printf("PASS cancellation: begin returns CANCELED; sweep resets RUNNING; "
           "next request computes fresh\n");
    return 0;
  }
  printf("FAIL cancellation\n");
  return 1;
}
