#ifndef ORE_DB_WORKSPACE_H
#define ORE_DB_WORKSPACE_H

#include <stdbool.h>
#include <stddef.h>

#include "../ids/ids.h"

struct db;

// =============================================================================
// SUBSTRATE BOUNDARY — what shares the source/file/namespace table vs
// what gets per-kind side tables. Read this before adding any new
// IR-shaped concept to the codebase.
//
// SOURCE-SHAPED (shared substrate, file = namespace 1:1:1):
//   - Disk-backed files (editor opens, lazy imports)
//   - Virtual files (comptime-emitted source, @build outputs,
//     @embedFile contents, macro expansions)
//   Distinguishing flag: db.sources.is_virtual[sid] + FileId virtual
//   bit (FILE_ID_VIRTUAL_BIT). All addressable via SourceId / FileId
//   / NamespaceId; all flow through file_ast / top_level_index /
//   namespace_type / type_of_def identically. Virtual files differ
//   only at I/O time (no realpath, no FS watcher, never inserted
//   into source_by_path).
//
// PER-DECL / PER-INSTANCE (separate side tables, NOT files):
//   - AST per file        — db.files.asts
//   - Type per def        — db.defs.types
//   - Body inference      — db.fns.slot_infer_*
//   - Body scopes per fn  — db.fns.slot_body_scopes_*
//   - FUTURE MIR per fn   — new db.fns.mir column when MIR lands
//   - FUTURE Monomorphized instances — IpKey-interned, NOT a file
//   - FUTURE Const-eval results — already in intern pool (IPK_INT_VALUE etc.)
//
// THE RULE: if you can write it as source code, it goes in the source
// table. If it's a derived IR, it gets its own per-kind side table.
// Don't try to express MIR or monomorphization as virtual files —
// they're not source-shaped, and forcing them through the file
// pipeline breaks the DOD column layout.
//
// COMPTIME INVALIDATION DOMAINS (forward-looking — not yet wired):
//   Comptime can read external state: env vars, filesystem reads
//   beyond the workspace, build flags, foreign tool output (@cImport),
//   system time, randomness. Each is a potential invalidation source
//   the salsa dep graph cannot see. Two paths the future comptime
//   engine can take:
//     (a) RESTRICT: comptime may only depend on tracked db inputs —
//         sources, intern-pool values, fingerprints of other queries.
//         @env / @system / @time forbidden or warn.
//     (b) TRACK: build a dep-recording mechanism that registers
//         comptime-read external state as inputs in the db.
//   Pick at comptime-implementation time. The comptime_call_cache
//   key must be flexible enough for either.
// =============================================================================

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

// External-tool-driven source update (FS watcher: file modified on
// disk by `git checkout`, codegen, another editor, etc.). Silently
// no-ops on unknown paths — the watcher fires for every file in the
// configured glob, not just ones we've lazy-loaded. See plan
// Phase 3a for the Roslyn/rust-analyzer model.
//
// If `text` is NULL, slurps the current disk content. Pass non-NULL
// text only when the caller already has the bytes in memory (rare —
// LSP watcher events don't carry content). Returns false if slurp
// failed (e.g., file was deleted between event and read).
bool workspace_did_change_external(struct db *s, const char *path,
                                   size_t path_len, const char *text,
                                   size_t text_len);

// External-tool-driven source removal (FS watcher: file deleted on
// disk). Marks the source row evicted + bumps DUR_MEDIUM. Source/
// File/Namespace IDs stay valid for the lifetime of the process
// (per the VFS-readiness commitment); downstream iteration skips
// evicted rows via db_get_source_evicted(sid) checks.
//
// V1 INVARIANT: this does NOT free the source text buffer (see plan
// Phase 3a "Eviction safety" note — db_resolve_span reads the text
// directly and a free here would UAF on cached diags rendered after
// the eviction). A V2 follow-up adds the free once all
// sources.texts readers gate on the evicted bit.
void workspace_did_evict_source(struct db *s, const char *path,
                                size_t path_len);

// Sema-callable. Resolves `path_str` (the literal arg of an @import)
// against the importer's file's directory and returns the imported
// file's NamespaceId, or NAMESPACE_ID_NONE if the target doesn't exist on
// disk. Lazy-loads from disk on cache miss (Roslyn/rust-analyzer
// "lazy inputs" model — disk reads populate a memoized view of
// immutable external state, no revision bump).
NamespaceId workspace_resolve_import(struct db *s, NamespaceId importer_module,
                                  StrId path_str);

// Admit an in-memory source as a first-class file. Used by future
// comptime that produces source (DSL parsers, codegen, @build
// outputs, @embedFile content). Returns SOURCE_ID_NONE on failure
// (e.g. on synthetic_name collision with an already-registered
// virtual or disk source).
//
// Identity rules (per plan Phase 3b):
//   - The returned SourceId is the ONLY way to reach this file.
//     Callers must hold the handle; there is no `@import("synthetic")`
//     path. synthetic_name is stored for debug/diag output only.
//   - sources.is_virtual = 1 on the new row. db_lookup_source_by_path
//     never returns virtual SourceIds.
//   - The file gets its own fresh NamespaceId (file = namespace 1:1,
//     Zig-aligned). Downstream queries (file_ast, top_level_index,
//     namespace_type, type_of_def, infer_body) are origin-agnostic
//     and treat the virtual file like any other.
SourceId workspace_admit_virtual(struct db *s,
                                 const char *synthetic_name,
                                 size_t name_len,
                                 const char *text, size_t text_len);

#endif // ORE_DB_WORKSPACE_H
