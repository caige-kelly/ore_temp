#ifndef ORE_DB_REQUEST_H
#define ORE_DB_REQUEST_H

#include <stdbool.h>
#include <stdint.h>

/*
    Request lifecycle — the LSP correctness layer.

    A "request" is one LSP interaction: hover, goto-definition, completion.
    Two properties of an in-flight request need protection from concurrent
    edits to be useful:

      1. Revision pinning. While a request computes, the user may type
         again. Without a pinned revision, in-flight queries would see
         partial new-revision state and produce torn results.
         db_request_begin stamps `request_revision` so query verification
         (slot.verified_rev compared against db_effective_revision())
         reads from the pinned value instead of the live current_revision.

      2. Cooperative cancellation. When the LSP shell learns a request
         is superseded, it flips db.cancel_requested. The query engine
         reads this at every db_check_cancel hook (called from query_begin
         in db/query/) and unwinds via QUERY_BEGIN_CANCELED.

    request_revision == 0 is the unpinned sentinel — db_effective_revision
    falls through to current_revision. Tests that don't care about pinning
    never call db_request_begin.

    db.request_arena is scratch storage for query bodies; db_request_begin
    and _end reset it. Comptime evaluations get their own per-call arenas
    (see comment in db.h above request_arena), so request_end's reset does
    not disturb comptime state — comptime values that need to survive the
    request boundary are interned into the InternPool.
*/

struct db;

// Begin a request: pin the revision, clear the cancel flag, reset the
// request scratch arena. `revision` must be nonzero. Asserts no nested
// request and an empty query stack.
void db_request_begin(struct db *s, uint64_t revision);

// End a request: unpin, reset the request scratch arena. Asserts the
// query stack is empty — a non-empty stack at the request boundary
// means a query body returned without calling query_succeed/fail.
void db_request_end(struct db *s);

// Cooperative cancel. Safe to call from any thread or signal handler —
// touches only the atomic flag.
void db_request_cancel(struct db *s);

// Read the cancel flag. Called from the query engine's begin hook;
// true means the in-flight request has been superseded and the caller
// should unwind without producing a result.
bool db_check_cancel(struct db *s);

// The revision the query engine validates cache slots against. Returns
// request_revision when a request is pinned, else current_revision.
uint64_t db_effective_revision(struct db *s);

extern uint64_t db_current_revision(struct db *s);
extern bool db_invalidation_enabled(struct db *s);


// Helpers for DB rev_control packed atomic
extern uint64_t db_current_revision(struct db *s);
extern bool db_invalidation_enabled(struct db *s);


#endif // ORE_DB_REQUEST_H
