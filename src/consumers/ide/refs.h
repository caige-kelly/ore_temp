#ifndef ORE_SEMA_REFS_H
#define ORE_SEMA_REFS_H

#include "../../common/arena.h"
#include "../../common/vec.h"
#include "../../parser/ast.h"
#include "../ids/ids.h"

// "Find references" via slot scan — RA-style scan-on-demand.
//
// Pre-cleanup this lived as a maintained reverse-index HashMap
// (Sema.refs_to_def) with refs_record / refs_unrecord calls inside
// every query_resolve_ref body. The maintenance was fragile: it
// handled "this Ident now resolves to a different def" correctly
// (record + unrecord on RECOMPUTE), but it had no answer for "this
// Ident disappears entirely" — the source-deleted resolve_ref slot
// never re-ran, so its prior contribution lingered in the reverse
// index forever, producing ghost references in LSP find-references.
//
// The fix follows rust-analyzer's approach (see
// crates/ide-db/src/search.rs FindUsages): don't maintain a
// reverse index. Walk every resolve_ref slot on demand, filter by
// `entry->def == target && state == QUERY_DONE`, return matching
// NodeIds. Slot eviction (when it lands) drops dead entries
// naturally; in the meantime, dead slots stay in DONE with their
// prior `entry->def` — but if the source no longer references the
// slot's NodeId, the slot is unreachable from the AST and its NodeId
// is no longer interesting to LSP find-references either. We accept
// the looseness; the alternative is per-slot bookkeeping that adds
// cost on every resolve to avoid a cost that's only paid once per
// find-references invocation.
//
// Cost: O(total resolve_ref slots) per call. find-references is
// human-paced; this is fine at single-file and small-project scale.
// If/when Ore grows to scale where this matters, add visibility-
// scoped pre-filtering (search_scope in RA's terminology).
//
// Path references (resolve_path slots) are NOT scanned here. The
// NodeId in a path slot's key points at the path head, not the
// segment that names the target — different semantics. Handle paths
// in a separate helper when find-references for path-tail segments
// becomes a need.

struct Sema;

// Collect every Ident NodeId that resolved to `def` via
// query_resolve_ref. Returns a Vec of struct NodeId allocated in
// `out_arena`. Returns NULL on invalid input. The returned Vec is
// sorted ascending by NodeId.id for deterministic output.
Vec *query_references_of(struct Sema *s, DefId def, Arena *out_arena);

#endif // ORE_SEMA_REFS_H
