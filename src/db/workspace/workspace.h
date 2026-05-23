#ifndef ORE_DB_WORKSPACE_H
#define ORE_DB_WORKSPACE_H

#include <stddef.h>

#include "../ids/ids.h"

struct db;

// The workspace coordinator is the SINGLE place that calls db setters
// for input management. Consumers (LSP, driver) and sema both call
// these functions; tracked queries stay strictly pure. When the
// Zig-style build system arrives, its discovery driver replaces the
// caller side of this API but the API itself stays.

// LSP/driver lifecycle.

// Register or update a source under `path`. Computes the file's
// owning module from dirname(path) via db_module_for_directory so
// sibling files share a ModuleId. If the source is new, also creates
// the file row; if it already exists, only the text is updated. Pass
// the path bytes (NOT a URI — convert via lsp_uri_to_path first).
void workspace_did_open(struct db *s, const char *path, size_t path_len,
                        const char *text, size_t text_len);

// Update the text of an already-registered source. No-op if the path
// is unknown (silent — matches LSP "stray didChange" handling; the
// driver would never call this).
void workspace_did_change(struct db *s, const char *path, size_t path_len,
                          const char *text, size_t text_len);

// Mark a source as no longer being edited. Source remains in the db
// (other files may import it). Gap B no-op stub; future LSP work may
// add reference-counting + eviction.
void workspace_did_close(struct db *s, const char *path, size_t path_len);

// Sema-callable. Resolves `path_str` (the literal arg of an @import)
// against `importer_module`'s root directory and returns the resolved
// ModuleId, or MODULE_ID_NONE if the path doesn't refer to a loaded
// source. Pure dispatch into db_query_module_for_path; Gap B does NOT
// trigger disk I/O here (consumers pre-load via workspace_did_open
// or workspace_enumerate_dir).
ModuleId workspace_resolve_import(struct db *s, ModuleId importer_module,
                                  StrId path_str);

#endif // ORE_DB_WORKSPACE_H
