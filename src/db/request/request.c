#include "request.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "../compact.h"
#include "../db.h"
#include "../query/invalidate.h"
#include "../query/query.h"
#include "../../support/data_structure/arena.h"
#include "../../support/data_structure/vec.h"

// Atomically update the request-revision bits of rev_control without
// stomping the current_revision or invalidation bits. CAS loop is the
// standard pattern for partial-field updates on a packed atomic — a
// load-then-store sequence is NOT atomic and would roll back any
// concurrent update to other bit-fields (e.g. an edit-thread bumping
// current_revision between our load and store).
static void rev_set_request(struct db *s, uint64_t request_bits) {
  uint64_t old = atomic_load(&s->rev_control);
  uint64_t new_val;
  do {
    new_val = (old & ~REV_REQUEST_MASK) | (request_bits & REV_REQUEST_MASK);
  } while (!atomic_compare_exchange_weak(&s->rev_control, &old, new_val));
}

void db_request_begin(struct db *s, uint64_t revision) {
  assert(s != NULL);
  assert(revision != 0 &&
         "revision 0 is the unpinned sentinel; pass current_revision or later");
  assert(s->query_stack.count == 0 &&
         "request begin while a query is still on the stack");

  rev_set_request(s, revision);
  atomic_store(&s->cancel_requested, false);
  arena_reset(&s->request_arena);

  // running_slots should be empty when a request starts — db_request_end
  // sweeps and clears it. The clear here is defensive in case a prior
  // request was aborted without reaching db_request_end (debugger, signal).
  vec_clear(&s->running_slots);
}

void db_request_end(struct db *s) {
  assert(s != NULL);
  assert(s->query_stack.count == 0 &&
         "request end while a query is still on the stack");

  // Defensive: reset any slot left in QUERY_RUNNING after the request.
  // The expected path is that every body reaches db_query_succeed/fail,
  // which transitions RUNNING → DONE/ERROR. This sweep catches future
  // patterns (cancellation, explicit early-return on cycle without
  // succeed) so a leftover RUNNING can't poison the next request's
  // cycle detection. See plan Phase 1f.
  for (size_t i = 0; i < s->running_slots.count; i++) {
    QueryRunningRef *ref = (QueryRunningRef *)vec_get(&s->running_slots, i);
    QuerySlotHot *slot = db_locate_slot(s, ref->kind, ref->key);
    if (slot && slot->state == QUERY_RUNNING) {
      slot->state = QUERY_EMPTY;
      slot->fingerprint = FINGERPRINT_NONE;
      // deps + diags left alone — they'll be cleared on next compute.
    }
  }
  vec_clear(&s->running_slots);

  rev_set_request(s, 0);
  arena_reset(&s->request_arena);

  // Mark-and-copy compaction across the shared salsa pools. Each
  // pool's trigger is gated on (count > MIN_THRESHOLD && count >
  // last_compacted * GROWTH_FACTOR) so short-running compile-once
  // sessions never compact, while long-running LSP sessions compact
  // roughly every doubling. This is the canonical safe point — every
  // query body has returned by here, so no Vec.data raw pointer
  // dereferenced inside a caller's frame can survive into the
  // relocation.
  db_pools_maybe_compact(s);
}

void db_request_cancel(struct db *s) {
  assert(s != NULL);
  atomic_store(&s->cancel_requested, true);
}

bool db_check_cancel(struct db *s) {
  assert(s != NULL);
  return atomic_load(&s->cancel_requested);
}

uint64_t db_effective_revision(struct db *s) {
  assert(s != NULL);
  uint64_t c = atomic_load(&s->rev_control);
  uint64_t r = c & REV_REQUEST_MASK;
  return r != 0 ? r : ((c & REV_CURRENT_MASK) >> 32);
}

uint64_t db_current_revision(struct db *s) {
  return (atomic_load(&s->rev_control) & REV_CURRENT_MASK) >> 32;
}

bool db_invalidation_enabled(struct db *s) {
  return (atomic_load(&s->rev_control) & REV_INVALIDATION_MASK) != 0;
}
