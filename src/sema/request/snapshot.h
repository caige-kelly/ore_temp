#ifndef ORE_SEMA_SNAPSHOT_H
#define ORE_SEMA_SNAPSHOT_H

#include <stdint.h>

// Snapshots — request-scoped revision pinning.
//
// The LSP correctness problem: the user types ("a"), the server
// dispatches a hover request that's still computing when the user
// types ("b"). If the second keystroke bumps current_revision and
// invalidates caches, the in-flight hover query starts seeing
// inconsistent state — some answers from the old revision, some
// from the new.
//
// Snapshots fix this. At request-handler entry we capture the
// current revision; the query engine reads from the snapshot's
// pinned revision instead of the global current_revision when
// deciding whether a slot is "verified up-to-date." The slot
// itself is single-revisioned today (we don't keep history), but
// the request sees a consistent answer because no slot it touches
// will be in a "post-snapshot revision" state.
//
// Mid-request mutations are still allowed (the LSP shell decides
// the policy) but they only invalidate caches *for the next
// request* — the in-flight one stays consistent.
//
// In single-threaded mode this is mostly a marker. Threading
// (later) requires copy-on-write or read-write locks; the
// snapshot API is the seam.

struct Sema;

struct Snapshot {
    struct Sema *s;
    uint64_t revision;
};

// Capture the current revision for the request. Stamps
// `s->request_revision` so the query engine reads from this
// pinned point. Nests: re-entering snapshot scope is supported
// (innermost wins; outer revision restored on end).
struct Snapshot sema_snapshot_begin(struct Sema *s);

// Release the snapshot. Restores any prior request_revision (or
// 0 if this was the outermost). Symmetric pairing with
// sema_snapshot_begin is the caller's responsibility.
void sema_snapshot_end(struct Snapshot *snap);

// The "effective revision" the engine should use for verification
// decisions. Wraps the request_revision-or-current_revision
// fallback so call sites don't repeat the if-check.
uint64_t sema_effective_revision(struct Sema *s);

#endif // ORE_SEMA_SNAPSHOT_H
