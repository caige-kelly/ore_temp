#ifndef ORE_DB_QUERY_DEF_IDENTITY_H
#define ORE_DB_QUERY_DEF_IDENTITY_H

#include "../db.h"

// Get-or-create the canonical DefId for (mid, ast_id). The DefId is
// allocated on first call and stored in a DefIdentityEntry inside the
// db.def_by_identity HashMap; subsequent calls (within or across
// db_query_module_exports re-runs) return the same DefId.
//
// On first allocation the body also fills the def's identity columns
// (db.defs.{names, ast_ids, parent_modules, meta, owner_scopes}) by
// resolving the AstId to its AST node via the module's files' AstIdMaps
// and reading the binding's extras.
//
// Downstream queries (type_of_def, fn_signature, const_eval) call this
// to obtain the DefId and to record their salsa dep on this slot's
// fingerprint, so editing one decl's identity invalidates only that
// decl's downstream chain.
//
// Returns DEF_ID_NONE if (mid, ast_id) cannot be resolved to a top-
// level binding in any of the module's files.
DefId db_query_def_identity(struct db *s, ModuleId mid, AstId ast_id);

#endif // ORE_DB_QUERY_DEF_IDENTITY_H
