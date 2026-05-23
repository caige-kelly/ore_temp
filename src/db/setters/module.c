// Module mutators — input boundary for the module table.
//
// A "module" is a thin aggregate over a set of files. The set of files
// belonging to a module is NOT stored on the module row — it's the
// back-ref `files.module_id` filtered down to M (see db_get_module_files).
//
// The old construction-only `db_add_file_to_module` is gone: the act
// of adding a file to a module IS db_create_file(src, mid), which
// stamps files.module_id[fid] = mid and stales the module's
// QUERY_TOP_LEVEL_INDEX slot. There is no separate "add" step.

#include "../db.h"

ModuleId db_create_module(struct db *s, StrId dir_path) {
  uint32_t idx = (uint32_t)s->modules.ids.count;
  ModuleId mid = {.idx = idx};
  // Grow every rowed modules column by one zero row in lockstep —
  // X-macro driven so a new (or split) column can't be forgotten.
#define X(name, type) vec_push_zero(&s->modules.name);
  ORE_MODULES_COLUMNS(X)
#undef X
  *(ModuleId *)vec_get(&s->modules.ids, idx) = mid;
  *(StrId *)vec_get(&s->modules.dirs, idx) = dir_path;
  return mid;
}

// Directory-as-module identity: two files in the same dir share a
// ModuleId. Sole policy point that future build systems will replace
// (e.g., with manifest-declared module identity), keyed the same way.
ModuleId db_module_for_directory(struct db *s, StrId dir_path) {
  // STR_ID_NONE is a sentinel; modules created with it (tests,
  // synthetic) bypass directory lookup. Each STR_ID_NONE call mints
  // a fresh module.
  if (dir_path.idx == STR_ID_NONE.idx)
    return db_create_module(s, dir_path);

  if (hashmap_contains(&s->module_by_directory, (uint64_t)dir_path.idx)) {
    void *slot =
        hashmap_get(&s->module_by_directory, (uint64_t)dir_path.idx);
    return (ModuleId){.idx = (uint32_t)(uintptr_t)slot};
  }
  ModuleId mid = db_create_module(s, dir_path);
  hashmap_put(&s->module_by_directory, (uint64_t)dir_path.idx,
              (void *)(uintptr_t)mid.idx);
  return mid;
}
