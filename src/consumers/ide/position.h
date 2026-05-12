#ifndef ORE_SEMA_POSITION_H
#define ORE_SEMA_POSITION_H

#include <stdbool.h>
#include <stdint.h>

#include "../../parser/ast.h"
#include "../ids/ids.h"

// Position-based queries — the entry-point family for LSP requests
// that start with "what's at line X col Y" cursor info.
//
// Backed by a per-module sorted span index (Vec<SpanIndexEntry>
// ordered by span start). Built lazily on first call;
// re-constructed when the module's AST re-parses (the entry's
// fingerprint shifts and the invalidation walker drops the cache).
//
// Lookup is binary search for the span containing (line, col),
// then a linear descent into nested children whose spans also
// contain the position. Result is the innermost NodeId — the
// "deepest interesting AST node" at that cursor.
//
// This forms the foundation for:
//   - goto-definition (resolve the NodeId, follow the DefInfo span)
//   - hover (resolve the NodeId, format type/doc info)
//   - autocomplete (use the NodeId's enclosing scope as the
//     completion source)
//   - inline rename (find all occurrences of the NodeId's def)

struct Sema;

// Find the innermost AST node whose span contains (line, col) in
// `mid`. Returns NodeId{0} when no node matches (cursor outside
// the module's content) or when the module hasn't been parsed.
//
// Lines and columns are 1-indexed to match LSP conventions.
struct NodeId query_node_at_position(struct Sema *s, ModuleId mid,
                                     uint32_t line, uint32_t col);

// Convenience: position → DefId. If the node at the position is an
// expr_Ident, resolve it via query_resolve_ref. Returns
// DEF_ID_INVALID when the position doesn't sit on a resolvable
// reference.
DefId query_def_at_position(struct Sema *s, ModuleId mid, uint32_t line,
                            uint32_t col);

#endif // ORE_SEMA_POSITION_H
