#ifndef ORE_DB_DIAG_H
#define ORE_DB_DIAG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "../ids/ids.h"
#include "../intern_pool/intern_pool.h"   // IpIndex
#include "../../support/data_structure/vec.h"
#include "../../syntax/syntax.h"          // SyntaxKind, SyntaxNode, SyntaxNodePtr
#include "../db.h"

struct db;


// ---- DiagAnchor ------------------------------------------------------
//
// Reparse-stable anchor for a diagnostic. Stores enough information to
// (a) directly render as a byte range via db_resolve_span (the
// captured-at-emit-time path), and (b) re-bind to a still-present
// SyntaxNode in the current tree via syntax_node_ptr_resolve (the
// reparse-stable path, used when fine-grained query memoization keeps
// a cached diag across edits that shifted byte offsets but did not
// touch the underlying node).
//
// 12 bytes natural packing. The `file_id` matches TinySpan's file
// encoding (16 bits). `(kind, start, length)` is the SyntaxNodePtr
// the anchor was captured from — `start, length` ARE TextRange,
// `kind` is SyntaxKind. Anchor length cap is 4 GB (uint32_t length);
// any node larger than that has bigger problems than diag rendering.
typedef struct {
    uint16_t   file_id;   // file_local index (matches TinySpan.file encoding)
    SyntaxKind kind;      // SyntaxKind of the anchored node (uint16_t)
    uint32_t   start;     // byte offset at emit time
    uint32_t   length;    // byte length at emit time
} DiagAnchor;

static_assert(sizeof(DiagAnchor) == 12, "DiagAnchor must stay 12 bytes");

#define DIAG_ANCHOR_NONE ((DiagAnchor){0})

static inline DiagAnchor diag_anchor_make(uint16_t file_id, SyntaxKind kind,
                                          uint32_t start, uint32_t length) {
    return (DiagAnchor){.file_id = file_id, .kind = kind,
                         .start = start, .length = length};
}

// Capture a diag anchor from a SyntaxNode in file `file_id` (file_local
// index). Reparse-stable: stores the node's kind + current byte range
// so render-time resolution can rebind via syntax_node_ptr_resolve.
static inline DiagAnchor diag_anchor_of_node(uint16_t file_id,
                                             const SyntaxNode *node) {
    SyntaxNodePtr ptr = syntax_node_ptr_new(node);
    return diag_anchor_make(file_id, ptr.kind, ptr.range.start,
                            ptr.range.length);
}

static inline bool diag_anchor_is_none(DiagAnchor a) {
    return a.file_id == 0 && a.kind == SYNTAX_KIND_NONE && a.start == 0 &&
           a.length == 0;
}

/*
    Light structured diagnostics.

    Architecture rationale:
    - Diagnostics are keyed by ANALYSIS UNIT — the (QueryKind, key) pair
      of the query that produced them. The DiagLists live in a dense
      db.diag_lists Vec; db.diags routes a unit key to a row. They are
      NOT stored on the query slot and NOT in a global DiagBag.
    - Unit keying gives Salsa-style early cutoff for free. A cached query's
      diags persist (its unit is untouched); a recomputed query's unit is
      cleared via db_diags_clear, called from db_query_begin before the
      body reruns. Because the key is independent of slot lifetime, a stale
      unit can also be cleared explicitly by an input setter — so
      broken→fixed→broken edits never drop or duplicate diagnostics.
    - Collection is O(emitted diags): db_collect_diags_* walk db.diags, not
      every query slot in the program.
    - Diag text is structured (template + args), not pre-formatted. The
      template is interned via db.strings — a given compiler message
      ("unexpected character {0}") dedupes to one StrId across every
      occurrence. Formatting deferred to render time means the LSP can
      collect 1000 diags and only format the ~10 visible-to-the-user
      ones. Filters become free.

    Storage (see the DiagList typedef in db.h):
      db.diag_lists   — Vec<DiagList>, one row per emitting unit (row 0
                        a reserved sentinel)
      db.diags        — HashMap<u64, u32>; unit key (kind, slot key) → row
      DiagList.items  — Vec<Diag> for one analysis unit
      DiagList.arena  — owns the byte-copied DiagArg payloads

    Lifetime: a unit's diags exist until db_diags_clear resets its
    DiagList (on recompute, or explicit input invalidation).
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
// 16 bytes total. Cache-friendly when args are walked sequentially.

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

// 16 bytes — same as the prior TinySpan-bearing layout. Secondary
// spans are DiagAnchor (12 bytes, 4-byte aligned), same reparse-stable
// encoding as the primary anchor, so related-info squiggles survive
// edits that shift byte offsets without touching the underlying node.
// All other union members are 4-byte aligned (StrId / IpIndex / int32 /
// uint32), so DiagAnchor's 4-byte alignment governs the union and the
// struct stays 16 B total.
typedef struct {
    DiagArgKind kind;
    uint8_t     _pad[3];
    union {
        uint32_t    ch;
        StrId       str;
        int32_t     i;
        IpIndex     type;
        DiagAnchor  span;
    };
} DiagArg;

static_assert(sizeof(DiagArg) == 16, "DiagArg must stay 16 B");


// ---- Diag struct -----------------------------------------------------
//
// 32 bytes. Vec<Diag> packs 2 per cache line; iterating diags during
// LSP publish or eviction is dense.
//
// `anchor` is a DiagAnchor (file:16 + kind:16 + start:32 + length:32 =
// 12 bytes). Reparse-stable: render-time resolution rebinds via
// syntax_node_ptr_resolve when the underlying SyntaxNode is still in
// the tree (the kind+length pair acts as the lookup key), and falls
// back to the captured byte range otherwise. This matters under fine-
// grained query memoization — a cached INFER_BODY diag for one fn
// must keep pointing at the right node even when the user edited
// whitespace in a sibling fn earlier in the file (which shifts byte
// offsets but leaves the cached fn's syntax tree identical).

typedef struct {
    DiagAnchor     anchor;       // 12 — reparse-stable anchor
    StrId          template_id;  //  4 — interned template; resolves via db.strings
    const DiagArg *args;         //  8 — borrowed; points into slot's diag_arena
                                 //       NULL when n_args == 0
    uint16_t       code;         //  2 — diagnostic code (0 = uncategorized;
                                 //       future use: linkable docs, suppression keys)
    uint8_t        n_args;       //  1
    DiagSeverity   severity;     //  1
    // 4 bytes trailing padding for 8-byte alignment of the args pointer
    // when Vec<Diag> packs entries. Total struct size 32 — still 2
    // entries per 64-byte cache line.
} Diag;

static_assert(sizeof(Diag) == 32, "Diag must stay 32 B (2 per cache line)");


// ---- Emit API --------------------------------------------------------
//
// Two flavors:
//
// db_emit: reads the analysis-unit key from the active query frame
//   (db_query_stack_top). Convenience for in-query emissions. Asserts
//   a frame is on the stack — emission outside a query is a contract
//   violation when using this entry.
//
// db_emit_to: takes an explicit (kind, key) for routing. Required for
//   post-typecheck orchestration code that emits outside any salsa
//   frame (e.g. sema_emit_unused_diagnostics walking refs after all
//   per-decl queries have closed).
//
// Both:
// - Lazy-create the unit's DiagList in db.diags on first emit
// - Auto-intern the (translated) template via db.strings
// - Copy args into the unit's arena (caller can pass stack-locals)
//
// Format specifiers in `fmt`:
//   %S       StrId         — interned string from db.strings
//   %s       const char *  — raw C string (auto-interned at emit time)
//   %T       IpIndex       — type formatted via db_format_type
//   %d       int32_t       — decimal integer
//   %c       uint32_t      — character (escaped if non-printable)
//   %P       DiagAnchor    — secondary location ("file:line:col")
//   %%       literal '%'
//
// Example:
//   db_emit(s, DIAG_ERROR, anchor, "no field '%S' in %T", fname, recv_ty);
//   db_emit_to(s, QUERY_NAMESPACE_SCOPES, nsid.idx, DIAG_WARNING,
//              anchor, "%S is unused", name);
//
// Max 8 args per call.
void db_emit(struct db *s, DiagSeverity severity, DiagAnchor anchor,
             const char *fmt, ...);
void db_emit_to(struct db *s, QueryKind unit_kind, uint64_t unit_key,
                DiagSeverity severity, DiagAnchor anchor,
                const char *fmt, ...);


// ---- Invalidation ----------------------------------------------------
//
// Clear one analysis unit's diagnostics (reset its DiagList). Called by
// the query engine from db_query_begin when a slot recomputes, and by
// input setters that stale an input query directly. No-op when the unit
// never emitted. `kind` / `key` are the same pair passed to db_query_begin.
void db_diags_clear(struct db *s, QueryKind kind, uint64_t key);


// ---- Collection ------------------------------------------------------
//
// Walk db.diags, copying Diag entries into out_diags. out_diags must be
// caller-initialized (vec_init, or vec_init_in_arena against a scratch
// arena). Cost is O(emitted diags), not O(query slots).
//
// The copy is shallow — Diag.args still points into the producing unit's
// DiagList arena. Callers consuming the collected diags must finish
// before any subsequent recompute of that unit could reset the arena.
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

// Resolve a reparse-stable anchor. Tries syntax_node_ptr_resolve
// against the file's current GreenNode root first — if the matching
// node is still present, uses its CURRENT byte range (so a cached diag
// emitted before a whitespace edit still squiggles the right node).
// Falls back to the captured (start, length) byte range when no
// matching node is found (graceful degradation for nodes the user
// edited away). Returns false when the file is evicted or unparsable.
bool db_resolve_anchor(struct db *s, DiagAnchor anchor, ResolvedSpan *out);


#endif // ORE_DB_DIAG_H
