#ifndef ORE_SEMA_LSP_ABI_H
#define ORE_SEMA_LSP_ABI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// LSP ABI — C-callable surface for the Rust tower-lsp shell.
//
// The Rust side links against the compiled Ore static/shared lib
// and calls these functions via FFI. Every function uses C ABI
// types only (no Vec, no Arena, no Sema struct directly) so the
// ABI is stable across compiler internal restructurings.
//
// Lifetimes:
//   - `OreDb*` is opaque; the only legal operations are the
//     functions declared here.
//   - Owned return values are documented per-function and must
//     be released with the matching `ore_*_destroy` /
//     `ore_*_release` call.
//   - Borrowed return values (e.g. `const char*` source text)
//     are valid until the next call that mutates the same
//     object; the Rust side typically copies into Rust-owned
//     memory before the next call.
//
// String encoding: UTF-8. Spans use 1-indexed lines/columns to
// match LSP. Slice/array returns use a {ptr, len} pair.

#ifdef __cplusplus
extern "C" {
#endif

// Opaque database handle — wraps a Sema instance plus the
// supporting state (arena, string pool, source map, diagnostics).
typedef struct OreDb OreDb;

// Opaque cancellation token — wraps a CancelToken. The Rust
// side creates one per request, calls `ore_db_cancel` to flip
// it, and frees it after the request completes.
typedef struct OreCancelToken OreCancelToken;

// === Lifecycle ===

// Create a fresh database. Returns NULL on allocation failure.
OreDb *ore_db_create(void);

// Destroy the database and release all owned resources (arena,
// pool, every cached query result). Safe to call with NULL.
void ore_db_destroy(OreDb *db);

// === Input management ===

// Register / refresh an input. `path` is the canonical file
// path (caller's responsibility to canonicalize). `text` is
// the current source bytes; pass NULL/0 to mark the input as
// "load from disk on next access."
//
// Bumps the database revision so downstream caches invalidate.
// Returns true on success.
bool ore_db_set_input(OreDb *db, const char *path, const char *text,
                      size_t text_len);

// Mark an input as having changed on disk without supplying new
// text. The next query that needs the source will reload it
// from `path`. Bumps revision.
bool ore_db_invalidate_input(OreDb *db, const char *path);

// === Position queries ===

// Span returned to the LSP shell. Lines/cols are 1-indexed.
// `file_id` matches the lexer's stamp for cross-referencing
// with the SourceMap.
typedef struct {
    int32_t file_id;
    int32_t start_line;
    int32_t start_col;
    int32_t end_line;
    int32_t end_col;
} OreSpan;

// Result of `ore_db_resolve_at` and `ore_db_definition_at`.
// `def_id` is opaque — pass it back to references / hover
// queries. `kind` is one of OreDefKind values.
typedef enum {
    ORE_DEF_NONE = 0,
    ORE_DEF_VALUE,
    ORE_DEF_TYPE,
    ORE_DEF_EFFECT,
    ORE_DEF_MODULE,
    ORE_DEF_PARAM,
    ORE_DEF_FIELD,
    ORE_DEF_PRIMITIVE,
} OreDefKind;

typedef struct {
    uint32_t def_id;
    OreDefKind kind;
    OreSpan defining_span;     // where the def itself sits
    OreSpan use_span;           // where the cursor was
    const char *name;           // borrowed from the database;
                                // valid until next mutation
} OreDefRef;

// Resolve the def at (line, col) in `path`. Returns a struct
// with `kind == ORE_DEF_NONE` when the cursor doesn't sit on
// a resolvable reference.
OreDefRef ore_db_resolve_at(OreDb *db, const char *path, uint32_t line,
                            uint32_t col);

// === References / hover / diagnostics ===

// Owned slice of OreSpan returned by ore_db_references / similar.
// Release with `ore_spans_release`.
typedef struct {
    OreSpan *items;
    size_t count;
} OreSpans;

void ore_spans_release(OreSpans *spans);

// Find every use of `def_id` across all loaded modules.
OreSpans ore_db_references(OreDb *db, uint32_t def_id);

// Diagnostic shape — one row per diagnostic. `code` is the
// E-code string ("E0100"); `severity` is 1=error, 2=warning,
// 3=info, 4=hint (matches LSP protocol). `message` is borrowed
// from the database arena.
typedef struct {
    uint32_t code;              // numeric portion, e.g. 100 for E0100
    int32_t severity;
    OreSpan span;
    const char *message;
} OreDiagnostic;

typedef struct {
    OreDiagnostic *items;
    size_t count;
} OreDiagnostics;

void ore_diagnostics_release(OreDiagnostics *diags);

// All diagnostics for `path`. Includes errors from parse,
// def_map, resolve, type-check (when wired). Empty list on
// success.
OreDiagnostics ore_db_diagnostics(OreDb *db, const char *path);

// === Cancellation ===

OreCancelToken *ore_cancel_token_create(void);
void ore_cancel_token_destroy(OreCancelToken *tok);

// Install / clear the active cancel token on `db`. Each
// request handler calls `set` at entry and `clear` at exit.
void ore_db_set_cancel(OreDb *db, OreCancelToken *tok);

// Flip the token to "cancelled." Safe to call from any thread.
// In-flight queries on the same db unwind via QUERY_BEGIN_CANCELED.
void ore_db_cancel(OreCancelToken *tok);

// === Snapshots ===

// Opaque snapshot handle. Created at request entry, destroyed at
// request exit. Threads through query calls implicitly via
// `db->request_revision`.
typedef struct OreSnapshot OreSnapshot;

OreSnapshot *ore_db_snapshot_begin(OreDb *db);
void ore_db_snapshot_end(OreSnapshot *snap);

#ifdef __cplusplus
}
#endif

#endif // ORE_SEMA_LSP_ABI_H
