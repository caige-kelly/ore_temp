#ifndef ORE_DB_DIAG_H
#define ORE_DB_DIAG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "../ids/ids.h"
#include "../intern_pool/intern_pool.h"   // IpIndex
#include "../storage/vec.h"
#include "../db.h"

struct db;

/*
    Light structured diagnostics.

    Architecture rationale:
    - Diagnostics live on the QUERY SLOT that produced them — NOT on a
      global DiagBag. This matches the rest of the data-oriented compiler
      shape: outputs sit next to the computation they're an output of.
    - Per-slot ownership gives Salsa-style early cutoff for free. A cached
      query's diags persist; a recomputed query's diags get wiped via
      arena_reset before the body reruns. This is the fix for bug_of_bugs
      R2 — without it, broken→fixed→broken edits dropped diagnostics on
      cache hit.
    - Diag text is structured (template + args), not pre-formatted. The
      template is interned via db.strings — a given compiler message
      ("unexpected character {0}") dedupes to one StrId across every
      occurrence. Formatting deferred to render time means the LSP can
      collect 1000 diags and only format the ~10 visible-to-the-user
      ones. Filters become free.

    Per-slot storage (already declared in QuerySlot, see query.h):
      slot->diag_arena   — chained Arena, lazy-init on first emit;
                           owns the Diag Vec, args byte buffers,
                           and any other diag-attached memory
      slot->diags        — Vec<Diag>, lazy-alloc'd inside diag_arena
      slot->diag_error_count — running count for fast "is this slot
                           in error state?" checks without walking

    Lifetime: diags exist as long as their owning slot's diag_arena
    isn't reset. Recompute resets it. Eviction (future) will too.
*/


// ---- Severity --------------------------------------------------------

typedef enum : uint8_t {
    DIAG_ERROR   = 0,
    DIAG_WARNING,
    DIAG_INFO,
    DIAG_HINT,
} DiagSeverity;


// ---- Template arg ----------------------------------------------------
//
// Templates use {0}, {1}, {2}, ... placeholders. Renderer substitutes
// from the args array by kind:
//   DIAG_ARG_CHAR   → printed as the character (escaped if non-printable)
//   DIAG_ARG_STR_ID → looked up via db.strings, printed as text
//   DIAG_ARG_INT    → printed as decimal integer
//   DIAG_ARG_TYPE   → formatted via ip_format
//   DIAG_ARG_SPAN   → printed as "file:line:col" (secondary location)
//
// 16 bytes total — TinySpan is the largest variant (12 B) plus
// 4 B for kind + pad. Cache-friendly when args are walked sequentially.

typedef enum : uint8_t {
    DIAG_ARG_NONE = 0,
    DIAG_ARG_CHAR,
    DIAG_ARG_STR_ID,
    DIAG_ARG_INT,        // i32 — diag args carry line numbers, counts, codes;
                         // 32-bit is enough. Add DIAG_ARG_INT64 if a real
                         // consumer needs more range.
    DIAG_ARG_TYPE,
    DIAG_ARG_SPAN,
} DiagArgKind;

// 16 bytes. Including a 64-bit int variant would push the struct to 24
// bytes via 8-byte alignment — not worth the size for diag args that
// are uniformly small integers in practice.
typedef struct {
    DiagArgKind kind;
    uint8_t     _pad[7];
    union {
        uint32_t    ch;
        StrId       str;
        int32_t     i;
        IpIndex     type;
        TinySpan span;
    };
} DiagArg;

static_assert(sizeof(DiagArg) == 16, "DiagArg must stay 16 B");


// ---- Diag struct -----------------------------------------------------
//
// 28 bytes. Vec<Diag> packs ~2 per cache line; iterating diags during
// LSP publish or eviction is dense.

typedef struct {
    TinySpan    primary;      // 12 — primary source location
    StrId          template_id;  //  4 — interned template; resolves via db.strings
    const DiagArg *args;         //  8 — borrowed; points into slot's diag_arena
                                 //       NULL when n_args == 0
    uint16_t       code;         //  2 — diagnostic code (0 = uncategorized;
                                 //       future use: linkable docs, suppression keys)
    uint8_t        n_args;       //  1
    DiagSeverity   severity;     //  1
    // 4 bytes trailing padding for 8-byte alignment of the args pointer
    // in a Vec<Diag>. Total struct size 32 — 2 entries per 64-byte
    // cache line.
} Diag;

static_assert(sizeof(Diag) == 32, "Diag must stay 32 B (2 per cache line)");


// ---- Emit API --------------------------------------------------------
//
// All emit functions:
// - Find the active query frame via db_query_stack_top
// - Locate that frame's slot via db_locate_slot(kind, key)
// - Lazy-init the slot's diag_arena on first call
// - Lazy-init the slot's diags Vec inside that arena
// - Auto-intern `template` via db.strings (cheap on repeats via pool dedup)
// - Copy args into the slot's diag_arena (caller can pass stack-locals)
// - Bump slot->diag_error_count when severity == DIAG_ERROR
//
// Caller is REQUIRED to be inside a query body — assert fires otherwise.
// Lexer/parser run inside QUERY_MODULE_AST's body, so the precondition
// holds for their hot paths.

// Zero-arg form. Template is the rendered message verbatim.
void db_emit_error  (struct db *s, TinySpan span, const char *tmpl);
void db_emit_warning(struct db *s, TinySpan span, const char *tmpl);
void db_emit_info   (struct db *s, TinySpan span, const char *tmpl);
void db_emit_hint   (struct db *s, TinySpan span, const char *tmpl);

// One-arg specializations for the common shapes.
void db_emit_error_c (struct db *s, TinySpan span,
                      const char *tmpl, uint32_t ch);
void db_emit_error_s (struct db *s, TinySpan span,
                      const char *tmpl, StrId str);
void db_emit_error_n (struct db *s, TinySpan span,
                      const char *tmpl, int32_t n);
void db_emit_error_t (struct db *s, TinySpan span,
                      const char *tmpl, IpIndex type);

// Two-StrId form — "expected {0}, got {1}".
void db_emit_error_ss(struct db *s, TinySpan span,
                      const char *tmpl, StrId a, StrId b);

// Vararg fallback for anything beyond the typed wrappers. Args are
// borrowed for the duration of the call; the emit function copies
// them into the slot's diag_arena before returning.
void db_emit_error_va(struct db *s, TinySpan span,
                      const char *tmpl,
                      const DiagArg *args, size_t n_args);

// Same patterns repeat for warning/info/hint via a generic core. Add
// typed variants for non-error severities when consumers need them.


// ---- Collection ------------------------------------------------------
//
// Walks every slot home in the db (via db_for_each_slot), copying their
// Diag entries into out_diags. out_diags must be caller-initialized
// (typically via vec_init or vec_init_in_arena against a scratch arena).
//
// The copy is shallow — Diag.args still points into the producing
// slot's diag_arena. Callers consuming the collected diags must finish
// before any subsequent slot RECOMPUTE could reset that arena.
//
// For LSP publish that runs at a quiescent point in a request, this is
// fine. For long-running consumers, copy out the args too.
void db_collect_diags_all(struct db *s, Vec *out_diags);

// Filtered collection — only diags whose primary.file matches `file`.
// Common pattern for LSP "publishDiagnostics" per-file messages.
void db_collect_diags_for_file(struct db *s, FileId file, Vec *out_diags);


// ---- Rendering -------------------------------------------------------
//
// Format one Diag into buf. snprintf-style return: number of bytes that
// WOULD have been written if buflen were unlimited; truncates to buflen
// and always NUL-terminates if buflen > 0.
//
// Resolves the template via db->strings, walks {N} placeholders,
// substitutes args by kind. Lives here rather than in a separate
// render module because it's a one-function dependency.
size_t db_format_diag(struct db *s, const Diag *d,
                      char *buf, size_t buflen);

// Streaming render — for diagnostics dumps where buffer sizing is awkward.
// Returns the number of bytes written; doesn't NUL-terminate.
size_t db_print_diag(struct db *s, const Diag *d, FILE *out);


// ---- Span resolution -------------------------------------------------
//
// Translate a TinySpan into its source-position context: file path,
// 1-indexed line + column, and the literal source line text. Used by
// db_print_diag for rust-style rendering and by external consumers
// (LSP, IDE bridges) that need (line, col) for protocol payloads.
//
// Returns false when the span can't be resolved (virtual file with no
// on-disk path, or a file whose line_starts haven't been built — e.g.
// the parse hasn't run). Callers fall back to whatever degenerate
// position their protocol requires (LSP: 0:0–0:0).
typedef struct {
  const char *path;
  uint32_t    line;          // 1-indexed
  uint32_t    col_start;     // 1-indexed
  uint32_t    col_end;       // 1-indexed, exclusive; clamped to the line
  const char *line_text;     // not NUL-terminated
  size_t      line_text_len;
} ResolvedSpan;

bool db_resolve_span(struct db *s, TinySpan span, ResolvedSpan *out);


#endif // ORE_DB_DIAG_H
