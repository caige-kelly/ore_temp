#ifndef ORE_SEMA_REFS_H
#define ORE_SEMA_REFS_H

#include "../../common/vec.h"
#include "../../parser/ast.h"
#include "../ids/ids.h"

// Reverse-reference index: DefId → Vec<NodeId> of every Ident that
// resolved to it via query_resolve_ref.
//
// This is the C-shaped translation of Salsa's "accumulator" pattern.
// Each `query_resolve_ref` slot owns a single accumulator
// contribution: the (def, ident_node) pair from its last successful
// resolution. The slot tracks what it contributed via
// `ResolveRefEntry.recorded_def`. On re-execution (after the
// invalidator forces a recompute), the body of `query_resolve_ref`:
//
//   1. Calls `refs_unrecord(prior_def, ident_node)` to drop its
//      previous contribution. ← This is what makes the index
//      consistent across edits; without it, a name that used to
//      resolve to `foo` and now resolves to `bar` would appear in
//      both lists.
//   2. Re-runs resolution.
//   3. Calls `refs_record(new_def, ident_node)` and updates
//      `recorded_def`.
//
// Used by:
//   - LSP "find references" — walks every NodeId, hands the LSP
//     the spans (looked up via the AST node).
//   - "Rename" — same iteration.
//   - Hover / "show callers" — same.
//
// The list is per-DefId, lifetime == Sema. Entries aren't deduped
// today (a single Ident can only resolve once, so no duplicates
// arise in practice).

struct Sema;

// Append `ident_node` to `def`'s reference list. No-op for invalid
// inputs. Called from resolve.c on every successful resolution —
// kept as its own helper so the resolution code stays focused on
// lookup logic.
void refs_record(struct Sema *s, DefId def, struct NodeId ident_node);

// Remove a single (def, ident_node) entry from the reverse index.
// No-op if the def has no list, or if the node isn't present.
//
// Called from `query_resolve_ref`'s COMPUTE path before the body
// re-resolves: any prior contribution this slot made gets dropped
// so re-resolution to a different def doesn't leave a stale entry
// behind. O(N) in the size of the def's ref list (linear scan +
// swap-remove); fine at typical scale (tens to hundreds of refs
// per def).
void refs_unrecord(struct Sema *s, DefId def, struct NodeId ident_node);

// Drop all recorded references for `def`. Useful when a def is
// removed entirely (e.g., a top-level decl deleted) — clears its
// ref list in one shot rather than per-(def, node) unrecord calls.
void refs_drop(struct Sema *s, DefId def);

// Return all recorded references to `def`, or NULL if none. The
// returned Vec is borrowed — caller may iterate but not mutate.
Vec *query_references_of(struct Sema *s, DefId def);

#endif // ORE_SEMA_REFS_H
