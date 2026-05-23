#ifndef ORE_DB_QUERY_BODY_SCOPES_H
#define ORE_DB_QUERY_BODY_SCOPES_H

#include "../db.h"

// Build the body-scope tree for `fn_def`. The result lives in
// db.fns.body[row] + the shared body-scope pools; this wrapper returns
// only success/failure — callers use it for the salsa dep + early
// cutoff, never for a value (see sema_body_scope_lookup for reads).
//
// Records deps on def_identity(nsid, ast_id), file_ast for each module
// file, and on the per-let-bind type queries (resolve_ref + type_of_def
// + fn_signature) that determine bind types. Any change to the body's
// AST shape or to a referenced decl's type invalidates this query.
bool db_query_body_scopes(struct db *s, DefId fn_def);

#endif // ORE_DB_QUERY_BODY_SCOPES_H
