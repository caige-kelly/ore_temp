// File mutators — the input boundary for the file table.
//
// A "file" is the parse unit: one source → one file → one module.
// db_create_file stamps the file's back-refs (source_id, module_id),
// prepares the per-file columns the parse query (QUERY_FILE_AST) will
// write into, and bumps the workspace-tier revision so dependent
// queries re-verify. Per-entry top-level enumeration is owned by
// QUERY_TOP_LEVEL_ENTRY now — no aggregating per-file index to stale.

#include "../db.h"

// First-chunk capacity for a per-file arena (db.files.arenas[fid]).
// Modest: most files are small; large ones grow via chunk doubling.
#define ORE_FILE_ARENA_DEFAULT_CAP (16 * 1024)

FileId db_create_file(struct db *s, SourceId src, NamespaceId owner) {
  uint32_t idx = (uint32_t)s->files.ids.count;
  FileId fid = file_id_make_physical(idx);

  // Grow every files column by one zero row in lockstep — X-macro
  // driven so a new (or split) column can't be forgotten here.
#define X(name, type, _evict) vec_push_zero(&s->files.name);
  ORE_FILES_COLUMNS(X)
#undef X
  // Stamp the identity / back-ref columns over their zero rows. The
  // owner write IS what makes this file a member of `owner`'s module —
  // db_get_namespace_files filters this column.
  *(FileId *)vec_get(&s->files.ids, idx) = fid;
  *(SourceId *)vec_get(&s->files.source_id, idx) = src;
  *(NamespaceId *)vec_get(&s->files.module_id, idx) = owner;
  arena_init((Arena *)vec_get(&s->files.arenas, idx),
             ORE_FILE_ARENA_DEFAULT_CAP);

  // O(1) source → file reverse index. Value is the file_local idx
  // (file_id_local of fid); callers reconstruct the FileId via
  // file_id_make_physical() in the lookup.
  hashmap_put(&s->file_by_source, (uint64_t)src.idx, (void *)(uintptr_t)idx);

  // Bump DUR_MEDIUM: the namespace's file set just grew, so any
  // namespace-scoped query (NAMESPACE_SCOPES, NAMESPACE_TYPE,
  // TOP_LEVEL_ENTRY) recorded against the old file set must re-verify
  // on its next pull. Per-entry verification then drives the actual
  // recompute decision — slots that were never computed pay nothing.
  if (namespace_id_valid(owner))
    db_input_changed(s, (uint8_t)DUR_MEDIUM);

  return fid;
}

// Virtual-file row: same shape as db_create_file but the FileId's
// virtual bit is set (file_id_make_virtual) and the gate-bump for
// TOP_LEVEL_INDEX is skipped — virtual files are admitted into a
// FRESH owner namespace (caller allocates via db_create_namespace
// right before), so the gate would never fire anyway.
//
// db_get_source_is_virtual(source(fid)) is the canonical "this file
// is synthetic" check; the FileId bit is the same information at the
// file layer.
FileId db_create_virtual_file(struct db *s, SourceId src, NamespaceId owner) {
  uint32_t idx = (uint32_t)s->files.ids.count;
  FileId fid = file_id_make_virtual(idx);

#define X(name, type, _evict) vec_push_zero(&s->files.name);
  ORE_FILES_COLUMNS(X)
#undef X
  *(FileId *)vec_get(&s->files.ids, idx) = fid;
  *(SourceId *)vec_get(&s->files.source_id, idx) = src;
  *(NamespaceId *)vec_get(&s->files.module_id, idx) = owner;
  arena_init((Arena *)vec_get(&s->files.arenas, idx),
             ORE_FILE_ARENA_DEFAULT_CAP);

  hashmap_put(&s->file_by_source, (uint64_t)src.idx, (void *)(uintptr_t)idx);
  // No TOP_LEVEL_INDEX gate-bump: virtual file's owner is always a
  // fresh namespace whose slot is QUERY_EMPTY.
  return fid;
}
