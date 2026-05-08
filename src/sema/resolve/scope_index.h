#ifndef ORE_SEMA_SCOPE_INDEX_H
#define ORE_SEMA_SCOPE_INDEX_H

#include "../../parser/ast.h"
#include "../ids/ids.h"

// Scope index — per-NodeId enclosing-scope map.
//
// The resolver needs to know, for any expression in the program,
// "which scope is this Ident in?" so it can start the parent-chain
// walk from the right place. The naive answer would be "walk down
// from the module each time an Ident is queried" — quadratic, and
// would re-execute scope construction on every lookup.
//
// Instead, we build the index once per module via a single AST walk
// that:
//   1. Records every NodeId's enclosing ScopeId.
//   2. Allocates SCOPE_FUNCTION/BLOCK/HANDLER/LOOP scopes as it
//      crosses scope-introducing AST nodes.
//   3. Allocates DefIds for params, local lets, and handler-op
//      parameters and inserts them into the right scope.
//
// query_scope_for_node is the read API; the build pass populates
// `Sema.node_to_scope`.
//
// Top-level binds are NOT re-registered here — def_map already
// owns module-scope decl registration. The walker for top-level
// expressions descends into RHS only, so a `main :: fn(...)` Bind
// emits the function-scope walk without inserting `main` into the
// internal scope a second time.

struct Sema;

// Build the scope index for one module. Idempotent via the slot
// pattern used by other queries — re-calls are cheap once the map
// is populated.
//
// Preconditions: query_module_def_map(s, mid) has succeeded for
// `mid`, so the module has internal_scope/export_scope and its
// top-level decls are registered.
void scope_index_build_module(struct Sema *s, ModuleId mid);

// Look up the innermost enclosing scope for `node`. Returns
// SCOPE_ID_INVALID if the node hasn't been indexed (which shouldn't
// happen for any AST node reachable from a built module — bug if it
// does).
ScopeId query_scope_for_node(struct Sema *s, struct NodeId node);

#endif // ORE_SEMA_SCOPE_INDEX_H
