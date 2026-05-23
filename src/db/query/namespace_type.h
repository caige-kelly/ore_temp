#ifndef ORE_DB_QUERY_NAMESPACE_TYPE_H
#define ORE_DB_QUERY_NAMESPACE_TYPE_H

#include "../db.h"
#include "../intern_pool/intern_pool.h"

// Build (or fetch the cached) IpIndex for the namespace's struct
// type. The fields are the file's public top-level decls; field
// TYPES are resolved lazily — the struct stores (StrId name, DefId
// def) pairs and field access at sema time calls db_query_type_of_def
// on demand.
//
// Matches Zig's Namespace.owner_type (each .zig file is itself a
// struct type whose decls are the fields).
//
// Deps:
//   - QUERY_TOP_LEVEL_INDEX(nsid) — fingerprint covers pub-decl set
//   - QUERY_DEF_IDENTITY(nsid, ast_id) per pub decl — DefIds are
//     stable across re-runs
//
// Returns IP_NONE on cycle (engine's DB_QUERY_GUARD CYCLE arg) or
// an invalid namespace. Otherwise returns the interned struct type
// IpIndex (cached in namespaces.exports[nsid].struct_type).
IpIndex db_query_namespace_type(struct db *s, NamespaceId nsid);

#endif // ORE_DB_QUERY_NAMESPACE_TYPE_H
