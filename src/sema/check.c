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
    uint32_t s0 = *(uint32_t *)vec_get(&s->scopes.decl_offsets, internal.idx);
    uint32_t s1 =
        *(uint32_t *)vec_get(&s->scopes.decl_offsets, internal.idx + 1);

    // 2. Materialize a stable DefId for every top-level decl, type it,
    //    and infer its body in a single pass. type_of_def delegates to
    //    fn_signature for fn-bound decls (no separate call needed);
    //    infer_body is a cheap no-op on non-fn defs.
    for (uint32_t i = s0; i < s1; i++) {
      DeclEntry *de = (DeclEntry *)vec_get(&s->scopes.decl_pool, i);
      DefId def = db_query_def_identity(s, nsid, de->ast_id);
      (void)db_query_type_of_def(s, def);
      (void)db_query_infer_body(s, def);
    }
  }

  // 3. Post-typecheck file-type walker — re-stamp FileNodeData.types[]
  //    for every AST node we know how to type. The per-decl queries
  //    above can early-cut (sibling-decl edits leave a decl's
  //    fingerprint unchanged), so their internal cache writes don't
  //    fire. The parser, meanwhile, zeroes types[] on every reparse.
  //    This walker closes the loop unconditionally — its salsa-hit-
  //    bounded cost is negligible vs the full typecheck. Without it,
  //    hover on a struct field after an unrelated edit shows `?`.
  uint32_t fc = 0;
  const FileId *files = db_get_namespace_files(s, nsid, &fc);
  for (uint32_t i = 0; i < fc; i++)
    sema_stamp_file_types(s, files[i]);
}
