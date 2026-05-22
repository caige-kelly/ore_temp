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

  vec_push(&s->files.ids, &fid);
  vec_push(&s->files.source_id, &src);
  vec_push(&s->files.module_id, &owner);
  vec_push_zero(&s->files.line_starts);
  vec_push_zero(&s->files.node_data);
  vec_push_zero(&s->files.node_counts);
  vec_push_zero(&s->files.arenas);
  arena_init((Arena *)vec_get(&s->files.arenas, idx),
             ORE_FILE_ARENA_DEFAULT_CAP);
  vec_push_zero(&s->files.asts);
  vec_push_zero(&s->files.trivia_tokens);
  vec_push_zero(&s->files.trivia_offsets);
  vec_push_zero(&s->files.ast_id_maps);
  vec_push_zero(&s->files.top_level_indices);
  vec_push_zero(&s->files.slots_ast);

  // O(1) source → file reverse index. Value is the file_local idx
  // (file_id_local of fid); callers reconstruct the FileId via
  // file_id_make_physical() in the lookup.
  hashmap_put(&s->file_by_source, (uint64_t)src.idx,
              (void *)(uintptr_t)idx);

  return fid;
}
