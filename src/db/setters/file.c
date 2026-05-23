// File mutators — the input boundary for the file table.
//
// A "file" is the parse unit: one source → one file → one module.
// db_create_file stamps the file's back-refs (source_id, module_id),
// prepares the per-file columns the parse query (QUERY_FILE_AST) will
// write into, and — because adding a file to a module is now atomic
// at this site — stales the owning module's QUERY_TOP_LEVEL_INDEX
// + bumps the workspace-tier revision.

#include "../db.h"
#include "../diag/diag.h"        // db_diags_clear — drop superseded index diags
#include "../query/invalidate.h" // db_locate_slot — for file-add invalidation

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
  // Stamp the identity / back-ref columns over their zero rows. The
  // owner write IS what makes this file a member of `owner`'s module —
  // db_get_module_files filters this column.
  *(FileId *)vec_get(&s->files.ids, idx) = fid;
  *(SourceId *)vec_get(&s->files.source_id, idx) = src;
  *(ModuleId *)vec_get(&s->files.module_id, idx) = owner;
  arena_init((Arena *)vec_get(&s->files.arenas, idx),
             ORE_FILE_ARENA_DEFAULT_CAP);

  // O(1) source → file reverse index. Value is the file_local idx
  // (file_id_local of fid); callers reconstruct the FileId via
  // file_id_make_physical() in the lookup.
  hashmap_put(&s->file_by_source, (uint64_t)src.idx, (void *)(uintptr_t)idx);

  // The module's file set just grew. If TOP_LEVEL_INDEX(owner) has
  // already been computed (slot in DONE/ERROR), stale it (same
  // mechanism db_set_source_text uses for QUERY_FILE_AST) so a
  // re-query reaggregates over the new file set, AND bump the
  // workspace-tier revision so dependents notice.
  //
  // The gate matters: during initial construction the slot is still
  // QUERY_EMPTY (no consumer has computed it yet), so there's nothing
  // to stale and no dependents to notify. Bumping unconditionally
  // would inflate revision numbers across cold setup with no effect.
  if (module_id_valid(owner)) {
    QuerySlotHot *sl =
        db_locate_slot(s, QUERY_TOP_LEVEL_INDEX, (uint64_t)owner.idx);
    if (sl && (sl->state == QUERY_DONE || sl->state == QUERY_ERROR)) {
      sl->state = QUERY_EMPTY;
      sl->fingerprint = FINGERPRINT_NONE;
      db_diags_clear(s, QUERY_TOP_LEVEL_INDEX, (uint64_t)owner.idx);
      db_input_changed(s, (uint8_t)DUR_MEDIUM);
    }
  }

  return fid;
}
