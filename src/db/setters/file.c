// File mutators — the input boundary for the file table.
//
// A "file" is the parse unit: one source → one file → one module
// (1:1 today, N:1 ready). db_create_file stamps the file's back-refs
// (source_id, module_id) and prepares the per-file columns the parse
// query (QUERY_FILE_AST) will write into.

#include "../db.h"

// First-chunk capacity for a per-file arena (db.files.arenas[fid]).
// Modest: most files are small; large ones grow via chunk doubling.
#define ORE_FILE_ARENA_DEFAULT_CAP (16 * 1024)

FileId db_create_file(struct db *s, SourceId src, ModuleId owner) {
  uint32_t idx = (uint32_t)s->files.ids.count;
  FileId fid = file_id_make_physical(idx);

  // Grow every files column by one zero row in lockstep — X-macro
  // driven so a new (or split) column can't be forgotten here.
#define X(name, type) vec_push_zero(&s->files.name);
  ORE_FILES_COLUMNS(X)
#undef X
  // Stamp the identity / back-ref columns over their zero rows.
  *(FileId *)vec_get(&s->files.ids, idx) = fid;
  *(SourceId *)vec_get(&s->files.source_id, idx) = src;
  *(ModuleId *)vec_get(&s->files.module_id, idx) = owner;
  arena_init((Arena *)vec_get(&s->files.arenas, idx),
             ORE_FILE_ARENA_DEFAULT_CAP);

  // O(1) source → file reverse index. Value is the file_local idx
  // (file_id_local of fid); callers reconstruct the FileId via
  // file_id_make_physical() in the lookup.
  hashmap_put(&s->file_by_source, (uint64_t)src.idx, (void *)(uintptr_t)idx);

  return fid;
}
