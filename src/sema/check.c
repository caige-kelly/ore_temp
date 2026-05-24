#include "../db/db.h"
#include "../db/intern_pool/intern_pool.h"
#include "../db/query/def_identity.h"
#include "../db/query/fn_signature.h"
#include "../db/query/infer_body.h"
#include "../db/query/module_exports.h"
#include "../db/query/type_of_def.h"
#include "sema.h"

// Run the full type-check pipeline for `nsid`. Caller is responsible
// for opening a salsa request (db_request_begin/end) around this call
// — sema is a consumer of the query engine and does NOT manage
// transactional boundaries. Each call is idempotent: cached slots
// short-circuit after the first run, so re-invoking inside the same
// request (or in a later request that finds nothing changed) is cheap.
void sema_check_module(struct db *s, NamespaceId nsid) {
  // 1. Build the module's internal + export scopes.
  (void)db_query_namespace_scopes(s, nsid);

  ScopeId internal = db_get_namespace_internal_scope(s, nsid);

  if (internal.idx != SCOPE_ID_NONE.idx) {
    uint32_t s0 = *(uint32_t *)vec_get(&s->scopes.decl_lo, internal.idx);
    uint32_t s1 = s0 + *(uint32_t *)vec_get(&s->scopes.decl_len, internal.idx);

    // Materialize a stable DefId for every top-level decl, type it,
    // and infer its body in a single pass. Each query (type_of_def,
    // infer_body) owns its own NodeTypesRange in db.node_types_pool;
    // the unified node_type router (db_query_node_type) reads from
    // those ranges. No post-typecheck walker required — the queries
    // are self-sufficient and salsa-cached. Sibling-decl edits leave
    // unrelated decls' fingerprints unchanged, so their ranges stay
    // valid across re-runs (Option-C: walker demolished).
    for (uint32_t i = s0; i < s1; i++) {
      DeclEntry *de = (DeclEntry *)vec_get(&s->scopes.decl_pool, i);
      DefId def = db_query_def_identity(s, nsid, de->ast_id);
      (void)db_query_type_of_def(s, def);
      (void)db_query_infer_body(s, def);
    }
  }

  // Tail-emit: walk the module's top-level decls and warn on any with
  // zero references (excluding pub exports and main). The ref_count
  // graph is fully settled by this point — every per-decl query above
  // has driven its resolve_ref calls.
  sema_emit_unused_diagnostics(s, nsid);
}
