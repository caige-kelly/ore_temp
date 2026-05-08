#ifndef ORE_SEMA_INPUTS_H
#define ORE_SEMA_INPUTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../query/query.h"

// Input layer — source text as a tracked, revision-aware input.
//
// In a Salsa-shaped database, every external thing that can change is
// modeled as an explicit "input." Queries are deterministic functions
// of inputs and other queries. When inputs change, downstream queries
// invalidate via fingerprint comparison.
//
// For a compiler, the inputs are file source texts. The LSP shell
// receives `textDocument/didChange` events and calls
// sema_set_input_source(); everything else (parse, def_map, resolve,
// typecheck) cascades from there.
//
// Identity & lifetime:
//   * Inputs are identified by `InputId`. ID idx==0 is invalid.
//   * One InputInfo per source file. Allocated arena-side, lifetime
//     == Sema lifetime.
//   * Path interning happens via StringPool — two registrations of
//     the same path return the same InputId.
//   * Source text lives either in the arena (after set_input_source)
//     or on disk (loaded on demand by query_input_source).
//
// Revision model:
//   * Every input mutation bumps `Sema.current_revision`.
//   * The input's `last_changed_rev` records when it last changed.
//   * Downstream slots store `verified_rev`/`computed_rev` and use
//     them with the input's revision to decide invalidation.
//
// This header is consumed by the modules layer (which holds a
// 1:1 InputId↔ModuleId mapping) and by future LSP code paths.

struct Sema;

typedef struct InputId { uint32_t idx; } InputId;

// === InputInfo ===
//
// One per registered source file. `path` is interned in the StringPool
// (the field stores the borrowed pointer for printing convenience —
// the canonical key is the pool's path_id, kept on Sema.inputs_by_path).
//
// `source` points to arena-owned bytes after sema_set_input_source.
// NULL means "load from disk on demand" — this is the initial state
// after sema_register_input; the LSP shell's first didOpen/didChange
// transitions it to the in-memory state.
//
// `source_fp` is the fingerprint of the *current* source. Two
// successive set_input_source calls with the same text produce the
// same fp, so the invalidation walker's early-cutoff sees no change
// and downstream caches stay warm.
//
// `last_changed_rev` is the global revision when the source last
// mutated. Module-level slots compare their `verified_rev` against
// this to decide whether to re-verify.
//
// `ast_query` is the query slot for query_module_ast — lives here
// (not on ModuleInfo) because the AST is a function of the input,
// not the module identity.
struct InputInfo {
    uint32_t path_id;             // StringPool key; canonical identity
    const char *path;             // borrowed view from the pool — for diagnostics
    char *source;                 // arena-owned; NULL = load from disk
    size_t source_len;
    Fingerprint source_fp;        // FNV-1a of the source bytes
    uint64_t last_changed_rev;
    bool is_dirty;                // true until next successful re-parse
    struct QuerySlot ast_query;
};

#define INPUT_ID_INVALID ((InputId){0})

static inline bool input_id_is_valid(InputId id) { return id.idx != 0; }
static inline bool input_id_eq(InputId a, InputId b) { return a.idx == b.idx; }

// Initialize the inputs subsystem. Pushes a NULL sentinel at slot 0
// of inputs_table so INPUT_ID_INVALID dereferences cleanly. Idempotent.
void sema_inputs_init(struct Sema *s);

// Register an input for `path`. Idempotent: if `path` is already
// registered (interned-string match), returns the existing InputId
// without bumping revision.
//
// Note: path canonicalization (realpath() resolution) is the caller's
// responsibility today. The LSP shell typically receives canonical
// URIs and converts them; the CLI driver registers what the user
// supplied. Two registrations of the same canonical path collapse
// to one InputId; non-canonical aliases register separately.
InputId sema_register_input(struct Sema *s, const char *path);

// Mutate the source text for an input. Bumps Sema's global revision,
// recomputes the source fingerprint, marks the input dirty so the
// next `query_module_ast` will re-parse.
//
// `text` is copied into the arena; the caller can free its buffer
// immediately after this returns.
//
// `len` is the byte length, exclusive of any null terminator. The
// stored copy is null-terminated for ergonomic interop.
//
// If `text` is NULL, treat as "input cleared" — equivalent to
// sema_invalidate_input followed by clearing the cached source.
void sema_set_input_source(struct Sema *s, InputId id, const char *text,
                           size_t len);

// Mark `id` as having changed on disk without supplying new text.
// Bumps revision and clears the in-memory source so the next query
// re-reads from disk. Used by file-watcher notifications when the
// LSP isn't authoritative for the buffer.
void sema_invalidate_input(struct Sema *s, InputId id);

// Resolve an input to its current source text. If the input has been
// set via `sema_set_input_source`, returns the cached text. Otherwise
// reads the file from disk on first call and caches it.
//
// Returns NULL on read failure (file missing, IO error, etc.) and
// emits an `E0001 input read failed` diagnostic. Callers that need
// to distinguish "absent" from "empty" should check the return.
const char *query_input_source(struct Sema *s, InputId id);
size_t query_input_source_len(struct Sema *s, InputId id);

// Accessor: returns the InputInfo at `id`, or NULL for invalid /
// out-of-range. Used by the module layer to thread the AST query
// slot. Not a stable public API — internal to sema.
struct InputInfo *input_info(struct Sema *s, InputId id);

#endif // ORE_SEMA_INPUTS_H
