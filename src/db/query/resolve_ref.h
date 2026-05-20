#ifndef ORE_DB_QUERY_RESOLVE_REF_H
#define ORE_DB_QUERY_RESOLVE_REF_H

#include "../db.h"

// Resolve `name` in `scope`'s decl_pool slice, falling through to
// `scope.parents[scope]` recursively until either a match is found or
// the root (SCOPE_ID_NONE) is reached. On hit, materializes the
// canonical DefId via db_query_def_identity(owning_module, ast_id),
// which records the appropriate dep so editing the resolved decl's
// identity invalidates this resolution.
//
// Cache slot lives in db.resolve_ref_cache keyed by (scope.idx << 32 |
// name.idx) — per-(scope, name) precision via HashMap.
//
// Returns DEF_ID_NONE if the name doesn't resolve to any decl visible
// from `scope` (parent chain walked).
DefId db_query_resolve_ref(struct db *s, ScopeId scope, StrId name);

#endif // ORE_DB_QUERY_RESOLVE_REF_H
