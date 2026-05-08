#ifndef ORE_SEMA_RESOLVE_H
#define ORE_SEMA_RESOLVE_H

#include "../../parser/ast.h"
#include "../ids/ids.h"
#include "../scope/scope.h"

// Reference & path resolution.
//
// Two distinct queries:
//
//   query_resolve_ref(ident_node, ns) — resolves a single Ident
//     in a namespace by walking the parent-scope chain from the
//     ident's enclosing scope (computed via query_scope_for_node)
//     up through module → prelude. At function-boundary scopes
//     it also consults the visible effect-op set via
//     query_effect_ops_visible.
//
//   query_resolve_path(start, segments[], ns) — resolves a
//     dotted path like `allocator.debug`. Each segment is
//     looked up in the *resolution scope*, then peeled into its
//     inhabitable scope (module → exports, type → members,
//     effect → ops). This is intentionally separate from struct
//     field-access lookups; conflating the two is a textbook
//     anti-pattern.
//
// Both queries cache by NodeId (the ident's NodeId, or the path
// root's NodeId for the path query) into Sema.resolved_refs.
//
// Sentinel: on miss / error / cycle, both queries return
// `s->error_def` (a stable DefId reserved at slot 0) so cascading
// callers don't crash on NULL. Diagnostics are emitted from
// resolve.c, deduped by (code, span).

struct Sema;

// Resolve `ident` (an expr_Ident) to a DefId in `ns`. The
// caller passes the Expr* so we can read its name without an
// auxiliary NodeId→Expr lookup. Cache key is ident->id.
//
// On miss, emits a "name not found" diagnostic (deduped by
// span+code) and caches the miss as DEF_ID_INVALID so repeated
// queries don't re-diagnose.
DefId query_resolve_ref(struct Sema *s, struct Expr *ident, Namespace ns);

// A path segment: one name + the source span where it was
// written. Built by callers from the AST shape (typically
// expr_Field chains rooted at an expr_Ident).
struct PathSegment {
    uint32_t name_id;
    struct Span span;
};

// Resolve `[start_scope] segments[]` to the def the path names.
// Returns DEF_ID_INVALID on miss / mid-path break (e.g., a
// segment names a non-module/non-type that has no inhabitable
// child scope). The `root_node` is used as the cache key.
DefId query_resolve_path(struct Sema *s, struct NodeId root_node,
                         ScopeId start_scope, const struct PathSegment *segments,
                         size_t segment_count, Namespace ns);

#endif // ORE_SEMA_RESOLVE_H
