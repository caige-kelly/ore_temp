#ifndef ORE_SEMA_SNAPSHOT_H
#define ORE_SEMA_SNAPSHOT_H

#include <stdint.h>

// Request-scoped revision pinning.
//
// The LSP correctness shape: while a long-running request (hover,
// definition) is computing, the user types again. Without a pinned
// revision, the in-flight request would see partial new-revision
// state from later edits. The fix is `Sema.request_revision` —
// when nonzero, query verification reads from it instead of the
// global current_revision.
//
// Today the LSP runs requests synchronously, so request_revision
// stays 0 and `sema_effective_revision` falls through to
// current_revision. When async handlers land, the request boundary
// will stamp request_revision at entry and clear at exit. This
// module is the seam.

struct Sema;

// The effective revision for verification decisions. Returns
// `request_revision` when pinned (nonzero), else `current_revision`.
// Live consumers: `sema_revalidate` (invalidate.c) and the
// last_accessed_rev touch in `sema_query_begin` (query.c).
uint64_t sema_effective_revision(struct Sema *s);

#endif // ORE_SEMA_SNAPSHOT_H
