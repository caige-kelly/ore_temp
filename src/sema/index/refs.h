#ifndef ORE_SEMA_REFS_H
#define ORE_SEMA_REFS_H

#include "../../common/vec.h"
#include "../../parser/ast.h"
#include "../ids/ids.h"

// Reverse-reference index: DefId → Vec<NodeId> of every Ident that
// resolved to it via query_resolve_ref.
//
// Populated as a side-effect of resolution: each successful
// query_resolve_ref call appends the ident's NodeId to the def's
// reference list. The bookkeeping lives in resolve.c via
// `refs_record(s, def, ident_node_id)`.
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

// Drop all recorded references for `def`. Called by the
// invalidation walker when `query_resolve_ref` is invalidated for
// any of `def`'s referencing nodes — avoids stale entries
// reappearing in `query_references_of` answers across edits.
//
// Today the invalidation walker doesn't yet wire this up; the
// helper exists so the future hook is one-liner-away.
void refs_drop(struct Sema *s, DefId def);

// Return all recorded references to `def`, or NULL if none. The
// returned Vec is borrowed — caller may iterate but not mutate.
Vec *query_references_of(struct Sema *s, DefId def);

#endif // ORE_SEMA_REFS_H
