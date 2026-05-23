// Workspace coordinator — the single layer that calls db input
// setters. See workspace.h for the layering rationale.

#include "workspace.h"

#include "../db.h"
#include "../query/module_for_path.h"
#include "../storage/stringpool.h"

// Compute dirname(path) length — everything up to (but not including)
// the last '/'. Returns 0 if there's no slash (caller treats as "").
static size_t dirname_len(const char *path, size_t path_len) {
  for (size_t i = path_len; i > 0; i--) {
    if (path[i - 1] == '/')
      return i - 1;
  }
  return 0;
}

void workspace_did_open(struct db *s, const char *path, size_t path_len,
                        const char *text, size_t text_len) {
  // Pin the file's owning module to dirname(path) so sibling files in
  // the same directory share a ModuleId (directory-as-module policy).
  size_t dl = dirname_len(path, path_len);
  StrId dir_id = pool_intern(&s->strings, path, dl);
  ModuleId mid = db_module_for_directory(s, dir_id);

  SourceId src = db_lookup_source_by_path(s, path, path_len);
  if (source_id_valid(src)) {
    // Already registered — just update the text. Source's module is
    // already pinned; db_set_source_text handles the FILE_AST stale
    // + revision bump.
    db_set_source_text(s, src, text, text_len);
    return;
  }

  src = db_create_source(s, path, path_len, text, text_len);
  (void)db_create_file(s, src, mid);
}

void workspace_did_change(struct db *s, const char *path, size_t path_len,
                          const char *text, size_t text_len) {
  SourceId src = db_lookup_source_by_path(s, path, path_len);
  if (!source_id_valid(src))
    return; // unknown source: silent no-op (LSP convention)
  db_set_source_text(s, src, text, text_len);
}

void workspace_did_close(struct db *s, const char *path, size_t path_len) {
  (void)s;
  (void)path;
  (void)path_len;
  // Source stays in the db: other files may still import it via the
  // resolved module. A future LSP could refcount editor-opens and
  // evict orphaned sources; not needed for Gap B.
}

ModuleId workspace_resolve_import(struct db *s, ModuleId importer_module,
                                  StrId path_str) {
  // Pure dispatch. Gap B requires consumers to pre-load all source
  // bytes before sema runs (via workspace_did_open or, eventually,
  // workspace_enumerate_dir). A MODULE_ID_NONE result means the file
  // wasn't registered — sema emits the diagnostic.
  return db_query_module_for_path(s, importer_module, path_str);
}
