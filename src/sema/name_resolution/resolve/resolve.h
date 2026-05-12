// #ifndef ORE_SEMA_RESOLVE_H
// #define ORE_SEMA_RESOLVE_H

// #include "../../parser/ast.h"
// #include "../ids/ids.h"
// #include "../query/query.h"
// #include "../scope/scope.h"
// #include "../../common/stringpool.h"

// // Reference & path resolution.
// //
// // Two distinct queries:
// //
// //   query_resolve_ref(ident_node, ns) — resolves a single Ident
// //     in a namespace by walking the parent-scope chain from the
// //     ident's enclosing scope (computed via query_scope_for_node)
// //     up through module → primitives. At function-boundary scopes
// //     it also consults the visible effect-op set via
// //     query_effect_ops_visible.
// //
// //   query_resolve_path(start, segments[], ns) — resolves a
// //     dotted path like `allocator.debug`. Each segment is
// //     looked up in the *resolution scope*, then peeled into its
// //     inhabitable scope (module → exports, type → members,
// //     effect → ops). This is intentionally separate from struct
// //     field-access lookups; conflating the two is a textbook
// //     anti-pattern.
// //
// // Caching: query_resolve_ref slot-caches per (NodeId, Namespace).
// // The same Ident node queried in NS_VALUE vs NS_TYPE gets distinct
// // cache entries — historically a single-slot cache here was a
// // latent correctness bug that namespace overloading would surface.
// //
// // Sentinel: on miss / error / cycle, both queries return
// // DEF_ID_INVALID. Diagnostics are emitted from resolve.c, deduped
// // by (code, span) once diag/codes.h ships.

// struct Sema;

// // Per-(NodeId, Namespace) cache entry for query_resolve_ref. Keyed
// // in Sema.resolve_ref_entries by `(NodeId<<4) | (uint64_t)ns`. The
// // entry owns its query slot; standard SEMA_QUERY_GUARD machinery
// // gives us cycle detection, dep recording, and (eventually) early
// // cutoff via fingerprint comparison.
// //
// // find-references consumers walk every slot in this table directly
// // (src/sema/index/refs.c) — RA-style scan-on-demand. There is no
// // maintained reverse index to keep consistent across edits, which
// // removes a whole class of "dead reference slot leaves ghost entry"
// // bugs (the resolve_ref slot for a deleted Ident never re-runs, so
// // its prior contribution would linger forever without slot eviction).
// struct ResolveRefEntry {
//     DefId def;
//     struct QuerySlot query;
// };

// // Per-(root NodeId, Namespace) cache entry for query_resolve_path.
// // Keyed in Sema.resolve_path_entries by `(NodeId<<4) | (uint64_t)ns`,
// // where NodeId is the AST node at the head of the path. Same shape
// // as ResolveRefEntry; the slot's deps are accumulated per segment
// // — each module-crossing segment calls query_module_exports for its
// // target module, recording the cross-module dep automatically via
// // the query stack.
// struct ResolvePathEntry {
//     DefId def;
//     struct QuerySlot query;
// };

// // Resolve `ident` (an expr_Ident) to a DefId in `ns`. The
// // caller passes the Expr* so we can read its name without an
// // auxiliary NodeId→Expr lookup. Cache key is ident->id.
// //
// // On miss, emits a "name not found" diagnostic (deduped by
// // span+code) and caches the miss as DEF_ID_INVALID so repeated
// // queries don't re-diagnose.
// DefId query_resolve_ref(struct Sema *s, struct Expr *ident, Namespace ns);

// // A path segment: one name + the source span where it was
// // written. Built by callers from the AST shape (typically
// // expr_Field chains rooted at an expr_Ident).
// struct PathSegment {
//     StrId name_id;
//     struct Span span;
// };

// // Resolve `[start_scope] segments[]` to the def the path names.
// // Returns DEF_ID_INVALID on miss / mid-path break (e.g., a
// // segment names a non-module/non-type that has no inhabitable
// // child scope). `root` is the head Expr of the path — used for
// // span/diag info and to derive the ExprId cache key.
// DefId query_resolve_path(struct Sema *s, struct Expr *root,
//                          ScopeId start_scope, const struct PathSegment *segments,
//                          size_t segment_count, Namespace ns);

// #endif // ORE_SEMA_RESOLVE_H

// src/resolve/resolve.h
#ifndef ORE_RESOLVE_H
#define ORE_RESOLVE_H

#include <stdint.h>
#include "../db/ids/ids.h" // Gives us GlobalNodeId

// The resolver needs to know if we are looking for a type or a value.
// e.g., `let x = Foo;` (NS_VALUE) vs `let x: Foo;` (NS_TYPE)
typedef enum {
    NS_VALUE = 0,
    NS_TYPE = 1,
    // NS_MACRO, etc...
} Namespace;

// The Query Key: This is what we pass to the DB to cache the result.
typedef struct {
    GlobalNodeId node;
    uint8_t namespace;
} ResolveRefKey;

// The Query Result: What does this reference point to?
typedef struct {
    DefId def;
    // other stuff like error states or multi-resolution candidates
} ResolveRefEntry;

// The actual query function that the rest of the compiler calls
struct db;
const ResolveRefEntry* query_resolve_ref(struct db* db, ResolveRefKey key);

#endif
