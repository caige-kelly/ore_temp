#ifndef ORE_DB_QUERY_BODY_SCOPES_H
#define ORE_DB_QUERY_BODY_SCOPES_H

#include "../db.h"

// Build the body-scope tree for `fn_def`. Returns a pointer to the
// BodyScopes stored in `db.defs.body_scopes[fn_def.idx]` (NULL for
// non-fn defs). The pointer is stable for the db's lifetime — the
// builder reuses + clears in-place on revalidation rather than
// reallocating, so callers may hold the pointer across cached calls.
//
// Records deps on def_identity(mid, ast_id), file_ast for each module
// file, and on the per-let-bind type queries (resolve_ref +
// type_of_def + fn_signature) that determine bind types. Any change
// to the body's AST shape or to a referenced decl's type invalidates
// this query.
//
// Sema-internal callers (during a body_scopes build for the SAME def,
// e.g. typing a let-bind's RHS expr) read `db.defs.body_scopes[def]`
// directly via sema_body_scopes_get — the partial state is exposed
// for self-recursion. External callers go through this wrapper to
// pick up the dep + cached early-cutoff.
BodyScopes *db_query_body_scopes(struct db *s, DefId fn_def);

#endif // ORE_DB_QUERY_BODY_SCOPES_H
