#ifndef ORE_SEMA_INVALIDATE_H
#define ORE_SEMA_INVALIDATE_H

#include <stdbool.h>

#include "query.h"

// Invalidation walker — Salsa-style early cutoff.
//
// When an input mutates (sema_set_input_source bumps the global
// revision), every DONE slot in the database is potentially stale.
// Naive recompute is wasteful; most slots' results are unchanged
// because their *recorded dep fingerprints* haven't shifted.
//
// The walker, called from sema_query_begin's CACHED path, decides
// per-slot:
//
//   if slot.verified_rev == current_revision:  reuse cached result
//   else: walk slot.deps; for each dep, recursively re-validate
//         the dep's slot; if any dep's current fingerprint differs
//         from the recorded `dep_fp`, the cached value is stale
//         and we recompute. Otherwise we mark slot.verified_rev =
//         current_revision and reuse.
//
// The "early cutoff" name comes from the optimization: if a function
// body changes but its TYPE SIGNATURE doesn't (e.g. you added a
// comment), the body's HIR slot recomputes but its
// `query_type_of_decl` slot's recorded dep_fp matches → callers of
// the fn skip recompute. Cascade stops at the first matching layer.

struct Sema;

typedef enum {
    REVALIDATE_SKIP_RECOMPUTE,   // dep tree unchanged; cached is fine
    REVALIDATE_RECOMPUTE,        // some dep changed; caller must recompute
    REVALIDATE_NOT_APPLICABLE,   // slot wasn't DONE — nothing to validate
} RevalidateResult;

// Resolve a (kind, key) pair to its slot pointer. Returns NULL when
// the kind doesn't currently have an addressable slot (e.g.,
// resolve_ref slots aren't keyed centrally). The walker treats NULL
// as "always fresh" — a non-addressable dep can't trigger a
// cascade. As more queries land their slot owners, the dispatch
// switch grows.
struct QuerySlot *sema_locate_slot(struct Sema *s, QueryKind kind,
                                   const void *key);

// Decide whether `slot` (presumed DONE) needs recomputation given
// the current revision and dep fingerprints. Updates
// slot->verified_rev if the slot is verified up-to-date.
//
// Recursive: re-validates each dep's slot before comparing
// fingerprints, so transitive change-propagation works correctly.
RevalidateResult sema_revalidate(struct Sema *s, struct QuerySlot *slot);

#endif // ORE_SEMA_INVALIDATE_H
