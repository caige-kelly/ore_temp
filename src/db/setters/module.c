// Module mutators — input boundary for the module table.
//
// A "module" is a thin aggregate over a set of files. Today every
// module has exactly one file (1:1), but the file_pool / file_offsets
// machinery is N:1-ready. db_add_file_to_module only supports
// appending to the most-recently-created module (callers create a
// module then add all its files before creating the next).

#include "../db.h"

#include <assert.h>

ModuleId db_create_module(struct db *s) {
  uint32_t idx = (uint32_t)s->modules.ids.count;
  ModuleId mid = {.idx = idx};
  // Grow every plain rowed modules column by one zero row in lockstep —
  // X-macro driven so a new (or split) column can't be forgotten. The
  // file_offsets/file_pool flat-pool pair is handled separately below.
#define X(name, type) vec_push_zero(&s->modules.name);
  ORE_MODULES_COLUMNS(X)
#undef X
  *(ModuleId *)vec_get(&s->modules.ids, idx) = mid;

  // The new module inherits the current file_pool end as its start.
  // Push the sentinel that marks the END of this module's (initially
  // empty) range. Invariant: file_offsets.count == module_count + 1.
  uint32_t end_offset = (uint32_t)s->modules.file_pool.count;
  vec_push(&s->modules.file_offsets, &end_offset);

  return mid;
}

void db_add_file_to_module(struct db *s, ModuleId mid, FileId fid) {
  assert(module_id_valid(mid));
  assert(mid.idx == s->modules.ids.count - 1 &&
         "db_add_file_to_module: only the last-created module is open");
  vec_push(&s->modules.file_pool, &fid);
  ((uint32_t *)s->modules.file_offsets.data)[mid.idx + 1] =
      (uint32_t)s->modules.file_pool.count;

  // A module's file SET is a DUR_MEDIUM structural input. Bump the
  // revision so QUERY_TOP_LEVEL_INDEX (which aggregates the file list)
  // and everything depending on it drop out of the durability fast-path
  // and re-derive. Without this the new dep edges would be inert — the
  // revision never advances on a file-set change.
  db_input_changed(s, DUR_MEDIUM);
}
