// File readers — accessors for the file-table SoA columns.

#include "../db.h"

SourceId db_get_file_source(struct db *s, FileId fid) {
  if (!file_id_valid(fid))
    return SOURCE_ID_NONE;
  uint32_t local = file_id_local(fid);
  if (local >= s->files.source_id.count)
    return SOURCE_ID_NONE;
  return *(SourceId *)vec_get(&s->files.source_id, local);
}

NamespaceId db_get_file_namespace(struct db *s, FileId fid) {
  if (!file_id_valid(fid))
    return NAMESPACE_ID_NONE;
  uint32_t local = file_id_local(fid);
  if (local >= s->files.module_id.count)
    return NAMESPACE_ID_NONE;
  return *(NamespaceId *)vec_get(&s->files.module_id, local);
}

// Source → File. O(1) via the file_by_source HashMap (populated by
// db_create_file). 1:1 source-to-file today; N:1 future would
// promote the value to a Vec<FileId> offset.
FileId db_lookup_file_by_source(struct db *s, SourceId src) {
  if (!source_id_valid(src))
    return FILE_ID_NONE;
  void *v = hashmap_get(&s->file_by_source, (uint64_t)src.idx);
  uint32_t idx = (uint32_t)(uintptr_t)v;
  if (idx == 0)
    return FILE_ID_NONE;
  return file_id_make_physical(idx);
}
