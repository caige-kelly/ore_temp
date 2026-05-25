// Module readers — accessors for the modules-table SoA.

#include "../db.h"
#include "../../support/data_structure/arena.h"

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
const FileId *db_get_namespace_files(struct db *s, NamespaceId nsid,
                                     uint32_t *out_count) {
  *out_count = 0;
  if (!namespace_id_valid(nsid))
    return NULL;

  size_t n = s->files.module_id.count;
  if (n <= 1) // only the row-0 sentinel
    return NULL;

  NamespaceId *mods = (NamespaceId *)s->files.module_id.data;
  FileId *fids = (FileId *)s->files.ids.data;
  SourceId *fsrcs = (SourceId *)s->files.source_id.data;

  // First pass: count matches that aren't evicted (skip row-0 sentinel).
  // Iteration filter — Phase 3a: evicted files are tombstones until
  // process restart; downstream queries treat the namespace as if
  // those files had never been registered.
  uint32_t count = 0;
  for (size_t i = 1; i < n; i++) {
    if (mods[i].idx != nsid.idx)
      continue;
    if (db_get_source_evicted(s, fsrcs[i]))
      continue;
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
    if (mods[i].idx != nsid.idx)
      continue;
    if (db_get_source_evicted(s, fsrcs[i]))
      continue;
    out[j++] = fids[i];
  }
  *out_count = count;
  return out;
}

// The module's internal scope — the QUERY_NAMESPACE_SCOPES result record
// (db.namespaces.exports). SCOPE_ID_NONE until that query first runs for
// the module. Plain column read; callers needing the scope BUILT must
// run db_query_namespace_scopes first.
//
// The export scope is gone — importers now go through the namespace
// struct type (db_query_namespace_type); the internal scope stays for
// resolving bare identifiers within this file.
ScopeId db_get_namespace_internal_scope(struct db *s, NamespaceId nsid) {
  if (!namespace_id_valid(nsid) || nsid.idx >= s->namespaces.exports.count)
    return SCOPE_ID_NONE;
  return ((NamespaceScopes *)vec_get(&s->namespaces.exports, nsid.idx))
      ->internal;
}
