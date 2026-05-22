#include "../db.h"
#include "index.h"
#include "query.h"
#include "query_engine.h"

// db_query_module_exports — builds the module's two scopes (internal
// scope = every decl; export scope = pub decls only) from each backing
// file's top_level_index and returns the EXPORT scope's ScopeId.
//
// Pure scope-builder: does NOT allocate DefIds and does NOT write to
// db.defs.*. Each DeclEntry holds only (name, ast_id); consumers route
// through db_query_def_identity(mid, ast_id) to obtain the canonical
// DefId. That query owns DefId allocation in a HashMap so re-runs of
// module_exports never re-allocate.
//
// Decl_pool growth invariant: only the most-recently-allocated scope
// can grow its slice. We allocate INTERNAL scope first (pushes
// everything), then EXPORT scope (pushes pubs from a transient stash).
//
// Dep recording: db_query_file_ast records a dep on each backing
// file's AST. The slot's fingerprint covers only the EXPORT scope's
// (name, ast_id, meta) tuples — private-decl edits do NOT change
// this fingerprint, so importers depending on this query stay
// early-cut.
ScopeId db_query_module_exports(struct db *s, ModuleId mid) {
  DB_QUERY_GUARD(
      s, QUERY_MODULE_EXPORTS, (uint64_t)mid.idx,
      ((ModuleExports *)vec_get(&s->modules.exports, mid.idx))->exported,
      SCOPE_ID_NONE, SCOPE_ID_NONE);

  // Lazy-alloc the internal scope on first run. The (internal, exported)
  // pair is the QUERY_MODULE_EXPORTS result record, db.modules.exports.
  ScopeId internal =
      ((ModuleExports *)vec_get(&s->modules.exports, mid.idx))->internal;
  if (internal.idx == SCOPE_ID_NONE.idx) {
    internal = db_create_scope(s);
    *(ScopeMeta *)vec_get(&s->scopes.meta, internal.idx) = SCOPE_MODULE;
    *(ModuleId *)vec_get(&s->scopes.owning_modules, internal.idx) = mid;
    ((ModuleExports *)vec_get(&s->modules.exports, mid.idx))->internal =
        internal;
  }

  // Depend on the module's top-level index — one aggregating query over
  // every backing file's parse. It also parses every file, so the
  // top_level_indices columns read below are populated. Depending on it
  // (rather than each raw QUERY_FILE_AST) means a body-only / comment
  // edit, which leaves the index fingerprint unchanged, lets this query
  // early-cut instead of rebuilding both scopes.
  (void)db_query_top_level_index(s, mid);

  uint32_t file_count = 0;
  const FileId *files = db_get_module_files(s, mid, &file_count);

  Vec pub_names, pub_ast_ids, pub_metas;
  vec_init(&pub_names, sizeof(StrId));
  vec_init(&pub_ast_ids, sizeof(AstId));
  vec_init(&pub_metas, sizeof(uint8_t));

  // Pass 1 — INTERNAL scope. The node→DefId reverse index is no longer
  // stamped here — it is its own query (QUERY_NODE_TO_DECL, see
  // query/node_to_def.c), since module_exports now early-cuts on
  // body-only edits and can't be relied on to re-stamp it.
  for (uint32_t fi = 0; fi < file_count; fi++) {
    FileId fid = files[fi];
    uint32_t local = file_id_local(fid);
    FileArray *idx = (FileArray *)vec_get(&s->files.top_level_indices, local);
    for (size_t i = 0; i < idx->count; i++) {
      TopLevelEntry *e = &((TopLevelEntry *)idx->data)[i];

      DeclEntry de = {.name = e->name, .ast_id = e->ast_id};
      vec_push(&s->scopes.decl_pool, &de);

      uint32_t new_end = (uint32_t)s->scopes.decl_pool.count;
      uint32_t *sentinel = (uint32_t *)vec_get(
          &s->scopes.decl_offsets, s->scopes.decl_offsets.count - 1);
      *sentinel = new_end;

      if ((e->meta & META_VIS_MASK) == VIS_PUBLIC) {
        vec_push(&pub_names, &e->name);
        vec_push(&pub_ast_ids, &e->ast_id);
        uint8_t mv = (uint8_t)e->meta;
        vec_push(&pub_metas, &mv);
      }
    }
  }

  // Pass 2 — EXPORT scope. Becomes the new most-recently-allocated
  // scope (owning the growing tail of decl_pool).
  ScopeId export_scope = db_create_scope(s);
  *(ScopeMeta *)vec_get(&s->scopes.meta, export_scope.idx) = SCOPE_MODULE;
  *(ModuleId *)vec_get(&s->scopes.owning_modules, export_scope.idx) = mid;
  ((ModuleExports *)vec_get(&s->modules.exports, mid.idx))->exported =
      export_scope;

  Fingerprint fp = db_fp_u64((uint64_t)pub_names.count);
  for (size_t i = 0; i < pub_names.count; i++) {
    StrId n = *(StrId *)vec_get(&pub_names, i);
    AstId a = *(AstId *)vec_get(&pub_ast_ids, i);
    uint8_t mv = *(uint8_t *)vec_get(&pub_metas, i);

    DeclEntry de = {.name = n, .ast_id = a};
    vec_push(&s->scopes.decl_pool, &de);

    uint32_t new_end = (uint32_t)s->scopes.decl_pool.count;
    uint32_t *sentinel = (uint32_t *)vec_get(&s->scopes.decl_offsets,
                                             s->scopes.decl_offsets.count - 1);
    *sentinel = new_end;

    fp = db_fp_combine(fp, db_fp_u64((uint64_t)n.idx));
    fp = db_fp_combine(fp, db_fp_u64((uint64_t)a.idx));
    fp = db_fp_combine(fp, db_fp_u64((uint64_t)mv));
  }

  vec_free(&pub_names);
  vec_free(&pub_ast_ids);
  vec_free(&pub_metas);

  db_query_succeed(s, QUERY_MODULE_EXPORTS, (uint64_t)mid.idx, fp);
  return export_scope;
}
