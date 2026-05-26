#ifndef ORE_DB_QUERY_DEF_IDENTITY_H
#define ORE_DB_QUERY_DEF_IDENTITY_H

#include "../db.h"

// Get-or-create the canonical DefId for (nsid, node_ptr). The DefId is
// allocated on first call and stored in a DefIdentityEntry inside the
// db.def_by_identity HashMap (keyed on (nsid.idx << 32 |
// syntax_node_ptr_hash(node_ptr))); subsequent calls within or across
// db_query_namespace_scopes re-runs return the same DefId.
//
// On first allocation the body also fills the def's identity columns
// (db.defs.{names, syntax_ptrs, parent_modules, meta}) by walking
// the module's files' top_level_indices and matching against
// node_ptr.
//
// Downstream queries (type_of_def, fn_signature, const_eval) call this
// to obtain the DefId and to record their salsa dep on this slot's
// fingerprint, so editing one decl's identity invalidates only that
// decl's downstream chain.
//
// Returns DEF_ID_NONE if (nsid, node_ptr) cannot be resolved to a
// top-level binding in any of the module's files.
DefId db_query_def_identity(struct db *s, NamespaceId nsid,
                            SyntaxNodePtr node_ptr);

#endif // ORE_DB_QUERY_DEF_IDENTITY_H
