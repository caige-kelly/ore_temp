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

// Register or update a source under `path`. Path is canonicalized
// via realpath() before registration, so /tmp/a, /private/tmp/a,
// and ./a (run from /private/tmp) all resolve to the same SourceId.
// Each registered file owns its own fresh NamespaceId (file-as-namespace,
// Zig-aligned — sibling files do NOT share scope). Returns the
// registered SourceId, or SOURCE_ID_NONE on failure. Pass the path
// bytes (NOT a URI — convert via lsp_uri_to_path first).
SourceId workspace_did_open(struct db *s, const char *path, size_t path_len,
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
// against the importer's file's directory and returns the imported
// file's NamespaceId, or NAMESPACE_ID_NONE if the target doesn't exist on
// disk. Lazy-loads from disk on cache miss (Roslyn/rust-analyzer
// "lazy inputs" model — disk reads populate a memoized view of
// immutable external state, no revision bump).
NamespaceId workspace_resolve_import(struct db *s, NamespaceId importer_module,
                                  StrId path_str);

#endif // ORE_DB_WORKSPACE_H
