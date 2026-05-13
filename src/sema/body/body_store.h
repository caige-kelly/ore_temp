#ifndef ORE_SEMA_BODY_STORE_H
#define ORE_SEMA_BODY_STORE_H

#include <stdint.h>

#include "../../support/common/hashmap.h"
#include "../../support/common/vec.h"
#include "../../db/ids/ids.h"
#include "../../db/query/query.h"

// Per-decl body store — R8.
//
// Assigns stable `ExprId = (decl, local)` identities to every body-
// level Expr reachable from `decl`'s body root. `local` is the index
// in a deterministic walk order; index 0 is a NULL sentinel so
// `local == 0` maps to EXPR_ID_NONE.
//
// Lifetime: one BodyStore per top-level (or nested-decl) DefId,
// owned by `Sema.body_stores`. Populated lazily by `query_body_store`
// — the slot guard caches the build, and the body's dep on
// `query_module_ast` forces a refresh on every source change.
//
// On RECOMPUTE the walk reuses `exprs` and `nodeid_to_local` (clears
// them first); on REVALIDATE_SKIP_RECOMPUTE everything stays put.
// The fingerprint folds `(kind, arity)` across the walk, so dependent
// body-level cache tables (type_of_expr, const_eval, …) get cutoff
// when the body's *shape* is unchanged even though `exprs` got
// refreshed with new `Expr*` values from a reparse.
//
// Nested decls (lambda inside an fn body) get their own BodyStore
// keyed off their own DefId — the enclosing store's walk stops at
// the bind/lambda Expr itself and doesn't descend.

struct Sema;
struct Expr;

struct BodyStore {
    DefId decl;
    Vec local_to_ast; 
    uint32_t ast_offset; // The file-level AstNodeId where this function begins
    Vec ast_to_local;    // Index with (AstNodeId - ast_offset) to get ExprId

    struct QuerySlot query;
};


// Build (or fetch) the body store for `decl`. Idempotent — slot
// guard handles caching. Returns NULL when `decl` has no body
// (e.g. DECL_FIELD, DECL_VARIANT, DECL_PRIMITIVE) or the lookup
// fails.
struct BodyStore *query_body_store(struct Sema *s, DefId decl);

// Resolve `expr` -> `ExprId`. Triggers the body store walk for
// `expr`'s owning decl on first call. Returns EXPR_ID_NONE for
// synthetic exprs (id.id == 0), unowned exprs, or unknown nodes.
//
// Side effect on first walk: populates `expr->expr_id` for every
// Expr in the owning decl's body, so subsequent calls hit the
// per-Expr cache instead of a HashMap.
ExprId expr_to_id(struct Sema *s, struct Expr *expr);

// Resolve `id` -> current-parse `Expr*`. Returns NULL on invalid
// id or out-of-range local.
struct Expr *id_to_expr(struct Sema *s, ExprId id);

#endif // ORE_SEMA_BODY_STORE_H
