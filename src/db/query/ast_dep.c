#include "ast_dep.h"

#include "../db.h"
#include "query.h"

// Resolve a DefId's owning ModuleId via the defs.parent_modules direct
// column — one cache-line read on this hot path. Returns MODULE_ID_NONE
// if the def is invalid or the column entry was never stamped.
static ModuleId owning_module_of(struct db *s, DefId def) {
  if (!def_id_valid(def))
    return MODULE_ID_NONE;
  if (def.idx >= s->defs.parent_modules.count)
    return MODULE_ID_NONE;
  ModuleId *mid = (ModuleId *)vec_get(&s->defs.parent_modules, def.idx);
  return mid ? *mid : MODULE_ID_NONE;
}

// Trigger an AST-dep recording on the current query's frame. The parse
// slot (QUERY_FILE_AST) is an LSP-managed input — db_query_begin
// returns CACHED and record_dep_on_parent (inside db_query_begin)
// stamps the dep on our caller's frame. The query key is the FileId by
// value — see db_query_begin.
static void record_dep_on_file(struct db *s, FileId fid) {
  uint32_t local = file_id_local(fid);
  if (local == 0 || local >= s->files.ids.count)
    return;
  (void)db_query_begin(s, QUERY_FILE_AST, (uint64_t)fid.idx);
  // Note: caller is responsible for being inside a query frame. If they
  // aren't, record_dep_on_parent inside db_query_begin no-ops (it checks
  // s->query_stack.count). No state cleanup needed here — db_query_begin's
  // cached path doesn't push a frame.
}

void db_record_ast_dep_for_def(struct db *s, DefId def) {
  if (!s)
    return;
  ModuleId mid = owning_module_of(s, def);
  if (!module_id_valid(mid))
    return;
  // Record the dep on each of the module's files' QUERY_FILE_AST.
  uint32_t n = 0;
  const FileId *files = db_get_module_files(s, mid, &n);
  for (uint32_t i = 0; i < n; i++)
    record_dep_on_file(s, files[i]);
}

void db_record_ast_dep_for_span(struct db *s, TinySpan span) {
  if (!s)
    return;
  // A span already carries its FileId — record the dep directly.
  record_dep_on_file(s, file_id_make_physical(span_file(span)));
}
