#ifndef ORE_SEMA_SCOPE_INDEX_H
#define ORE_SEMA_SCOPE_INDEX_H

#include "../../common/hashmap.h"
#include "../../common/vec.h"
#include "../../parser/ast.h"
#include "../ids/ids.h"
#include "../query/query.h"

// Scope index — per-NodeId enclosing-scope map, split for laziness.
//
// Two queries cooperate:
//
//   query_node_to_decl_index(mid)
//     Eager-once-per-module shallow walk that records every NodeId
//     in the module's subtree → its enclosing top-level DefId.
//     Cheap: no scope creation, no DefId allocation. Populates the
//     global Sema.node_to_decl hashmap.
//
//   query_fn_scope_index(fn_def)
//     Per-fn deep walk that creates SCOPE_FUNCTION/BLOCK/HANDLER/
//     LOOP scopes, allocates DefIds for params and locals, and
//     populates a per-fn node_to_scope hashmap. Stored in
//     Sema.fn_scope_index_cache. Editing one fn's body invalidates
//     this entry only — other fns' caches stay warm.
//
// query_scope_for_node combines them: look up the enclosing decl,
// trigger that fn's scope_index if not yet built, return the scope.
//
// This split is what makes the architecture LSP-grade. A keystroke
// inside main() never re-walks foo()'s body for scope answers, and
// vice versa.

struct Sema;

// Per-fn scope construction result. Owns the per-fn `node_to_scope`
// map and the list of scopes the walk created. Lifetime is the
// Sema's arena.
struct ScopeIndexResult {
    DefId fn_def;
    Vec *local_scopes;            // Vec<ScopeId>
    HashMap node_to_scope;        // NodeId.id -> ScopeId.idx packed
    struct QuerySlot query;
};

// Build the per-module shallow index (NodeId → enclosing top-level
// DefId). Eager once per module via the input's slot. Updates
// Sema.node_to_decl globally.
//
// Preconditions: query_module_def_map has run for `mid` so DefIds
// exist for every top-level decl.
void query_node_to_decl_index(struct Sema *s, ModuleId mid);

// Lookup: NodeId → enclosing top-level DefId. Returns
// DEF_ID_INVALID if the node is in a module whose index hasn't
// been built — caller should have triggered the per-module index
// for the relevant module first.
DefId query_node_to_decl(struct Sema *s, struct NodeId node);

// Build (or fetch cached) per-fn scope index for `fn_def`. Walks
// the fn's subtree, creates local scopes, allocates param/local
// DefIds, populates per-fn node_to_scope. Returns NULL on error.
struct ScopeIndexResult *query_fn_scope_index(struct Sema *s, DefId fn_def);

// High-level: NodeId → innermost ScopeId. Combines the two queries.
// Returns SCOPE_ID_INVALID for unindexed nodes (bug in caller).
ScopeId query_scope_for_node(struct Sema *s, struct NodeId node);

// Batch convenience: index every module-level decl, then build
// every fn's scope_index. Used by `--dump-scopes` and tests.
// Equivalent to lazy mode but eager — same final cache state.
void scope_index_build_module(struct Sema *s, ModuleId mid);

#endif // ORE_SEMA_SCOPE_INDEX_H
