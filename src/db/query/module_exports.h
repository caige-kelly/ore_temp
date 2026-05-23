#ifndef ORE_DB_QUERY_MODULE_EXPORTS_H
#define ORE_DB_QUERY_MODULE_EXPORTS_H

#include "../db.h"

// Build (or return cached) the module's two scopes and return the
// EXPORT scope's ScopeId. Slot key = &db.namespaces.ids[nsid.idx]
// (pointer-stable into the Vec). DefIds are allocated eagerly here;
// each DeclEntry holds its DefId. defs.* identity columns are filled
// lazily by db_query_def_identity(def) on first call.
ScopeId db_query_namespace_scopes(struct db *s, NamespaceId nsid);

#endif // ORE_DB_QUERY_MODULE_EXPORTS_H
