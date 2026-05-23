#ifndef ORE_DB_QUERY_MODULE_FOR_PATH_H
#define ORE_DB_QUERY_MODULE_FOR_PATH_H

#include "../ids/ids.h"

struct db;

// QUERY_MODULE_FOR_PATH — pure resolution from (importer_module,
// relative path string) to a ModuleId.
//
// Resolves `path_str` against `importer_module`'s root directory
// (lexical normalization; no disk I/O), looks up the canonical path
// in db.source_by_path, and returns the owning file's module.
//
// Returns MODULE_ID_NONE if:
//   - the path doesn't resolve (no source registered at that path —
//     the workspace should have pre-loaded it)
//   - the importer module has no root directory (STR_ID_NONE in
//     modules.dirs)
//   - the importer module is invalid
//
// This query is PURE: no setters, no I/O, no fs callback. Sema calls
// it via workspace_resolve_import, which handles the "load the file
// first if it's missing" lazy-load. When the build system lands, this
// query's body grows to also handle named imports + boundary checks,
// but its signature and call sites do not change.
ModuleId db_query_module_for_path(struct db *s, ModuleId importer_module,
                                  StrId path_str);

#endif // ORE_DB_QUERY_MODULE_FOR_PATH_H
