// Module readers — accessors for the modules-table SoA.

#include "../../support/data_structure/arena.h"
#include "../db.h"

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
  if (!namespace_id_valid(nsid) || nsid.idx >= s->namespaces.member_files.count)
    return NULL;

  // Per-namespace reverse index (maintained in file_set_add /
  // db_namespace_remove_file): the files admitted to this namespace, in
  // admission order. Reading it is O(files-in-namespace), not O(all files
  // in the workspace) — the S1 scaling fix. The evicted filter below is now
  // DEFENSIVE: db_namespace_remove_file already drops evicted files from
  // member_files, so this only guards a hypothetical future eviction-bit
  // setter that doesn't route through that removal.
  const Vec *list = (const Vec *)vec_get(&s->namespaces.member_files, nsid.idx);
  const FileId *members = (const FileId *)list->data;

  // First pass: count non-evicted.
  uint32_t count = 0;
  for (size_t i = 0; i < list->count; i++)
    if (!db_get_source_evicted(s, db_get_file_source(s, members[i])))
      count++;
  if (count == 0)
    return NULL;

  // Second pass: copy survivors into request_arena. Bump-pointer alloc,
  // freed wholesale at next db_request_begin.
  FileId *out = (FileId *)arena_alloc_raw(&s->request_arena,
                                          (size_t)count * sizeof(FileId));
  uint32_t j = 0;
  for (size_t i = 0; i < list->count; i++)
    if (!db_get_source_evicted(s, db_get_file_source(s, members[i])))
      out[j++] = members[i];
  *out_count = count;
  return out;
}
