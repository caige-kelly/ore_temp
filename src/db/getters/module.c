// Module readers — accessors for the modules-table SoA.

#include "../db.h"
#include "../storage/arena.h"

#include <string.h>

// "Files in module M" — filter scan over the dense files.module_id
// back-ref column. There is no per-module file list to look up; the
// owner write in db_create_file IS the membership record.
//
// Result is allocated in db.request_arena: the slice is valid through
// the end of the current request and reclaimed by the next
// db_request_begin. Callers that need a longer lifetime must copy.
//
// Hot-path consumer is db_query_top_level_index, which is itself
// cached — the scan only runs on recompute. For typical workspace
// sizes (a few hundred to a few thousand files), the dense u32 scan
// is single-digit microseconds.
const FileId *db_get_module_files(struct db *s, ModuleId mid,
                                  uint32_t *out_count) {
  *out_count = 0;
  if (!module_id_valid(mid))
    return NULL;

  size_t n = s->files.module_id.count;
  if (n <= 1) // only the row-0 sentinel
    return NULL;

  ModuleId *mods = (ModuleId *)s->files.module_id.data;
  FileId *fids = (FileId *)s->files.ids.data;

  // First pass: count matches (skip row-0 sentinel).
  uint32_t count = 0;
  for (size_t i = 1; i < n; i++) {
    if (mods[i].idx == mid.idx)
      count++;
  }
  if (count == 0)
    return NULL;

  // Second pass: copy into request_arena. Bump-pointer alloc, freed
  // wholesale at next db_request_begin.
  FileId *out = (FileId *)arena_alloc_raw(&s->request_arena,
                                          (size_t)count * sizeof(FileId));
  uint32_t j = 0;
  for (size_t i = 1; i < n; i++) {
    if (mods[i].idx == mid.idx)
      out[j++] = fids[i];
  }
  *out_count = count;
  return out;
}

// The module's export / internal scope — the QUERY_MODULE_EXPORTS
// result record (db.modules.exports). SCOPE_ID_NONE until that query
// first runs for the module. Plain column reads; callers that need the
// scopes to be BUILT must run db_query_module_exports first.
ScopeId db_get_module_export_scope(struct db *s, ModuleId mid) {
  if (!module_id_valid(mid) || mid.idx >= s->modules.exports.count)
    return SCOPE_ID_NONE;
  return ((ModuleExports *)vec_get(&s->modules.exports, mid.idx))->exported;
}

ScopeId db_get_module_internal_scope(struct db *s, ModuleId mid) {
  if (!module_id_valid(mid) || mid.idx >= s->modules.exports.count)
    return SCOPE_ID_NONE;
  return ((ModuleExports *)vec_get(&s->modules.exports, mid.idx))->internal;
}
