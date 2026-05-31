#ifndef ORE_DB_DIAG_H
#define ORE_DB_DIAG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "../ids/ids.h"
#include "../intern_pool/intern_pool.h"   // IpIndex
#include "../../support/data_structure/arena.h"  // Arena (Phase P DiagBundle)
#include "../../support/data_structure/vec.h"
#include "../../syntax/syntax.h"          // SyntaxKind, SyntaxNode, SyntaxNodePtr
#include "../db.h"

struct db;


// ---- AstId infrastructure (Phase P) ----------------------------------
//
// Stable per-file and per-body node identifiers, decoupled from absolute
// byte offsets. See plan file Phase P — P1 for the full design.
//
// FileAstId  : index into the file's FileAstIdMap.ptrs (preorder walk
//              over file-scoped nodes: decls, items, parse-error nodes).
// RelAstId   : index into a fn body's BodyAstIdMap.ptrs (preorder over
//              the body subtree).
// DeclKey    : low 32 bits of the def_identity hash. uint32_t (not u64)
//              keeps DiagAnchor at exactly 16 B; collision analysis at
//              ~2^16 decls per namespace bounds the risk and the per-
//              file index gates cross-namespace collisions naturally.
typedef uint32_t FileAstId;
typedef uint32_t RelAstId;
typedef uint32_t DeclKey;


// ---- DiagAnchor (Phase P v2) -----------------------------------------
//
// Tagged-union anchor with three resolve paths:
//
//   DIAG_ANCHOR_FILE_RAW : legacy path. Carries raw (file_id, start,
//     length) — exactly what the pre-Phase-P 12 B anchor carried. Used
//     by the parser / lexer where no AstIdMap exists yet, and by every
//     emit site that hasn't yet migrated to a structural anchor.
//   DIAG_ANCHOR_FILE     : (file_id, FileAstId) — resolves through
//     `db.files.ast_id_maps[file_id]` at publish time. Survives any
//     reparse that doesn't change the file's structural shape.
//   DIAG_ANCHOR_BODY     : (DeclKey, RelAstId) — resolves through
//     `db.fns.body_ast_id_maps[def]` at publish time. Survives sibling
//     reparses (cached body diags resolve through the body's stable
//     local map paired with the def_identity-stable DeclKey).
//
// 16 bytes exact. See plan P1.c for the alignment math that fixes
// DeclKey at uint32_t.
typedef enum {
    DIAG_ANCHOR_NONE_KIND = 0,
    DIAG_ANCHOR_FILE_RAW,   // {file_id, start, length} — legacy / pre-parse
    DIAG_ANCHOR_FILE,       // {file_id, FileAstId}
    DIAG_ANCHOR_BODY,       // {DeclKey, RelAstId}
} DiagAnchorKind;

typedef struct {
    uint8_t   kind;       // 1  DiagAnchorKind
    uint8_t   _pad[3];    // 3
    uint32_t  file_id;    // 4  FILE / FILE_RAW only (zero for BODY)
    union {
        FileAstId  file_ast_id;                              // 4
        struct {
            uint32_t start;
            uint32_t length;
        } raw;                                               // 8
        struct {
            DeclKey  decl;
            RelAstId rel;
        } body;                                              // 8
    } u;                  // 8 (all variants, 4-byte aligned)
} DiagAnchor;

static_assert(sizeof(DiagAnchor) == 16, "DiagAnchor must stay 16 bytes");
static_assert(_Alignof(DiagAnchor) == 4, "DiagAnchor must stay 4-byte aligned");

#define DIAG_ANCHOR_NONE ((DiagAnchor){0})

// Legacy constructor — produces a FILE_RAW variant. All pre-Phase-P call
// sites construct anchors via this helper, so the shape change preserves
// their semantics exactly.
static inline DiagAnchor diag_anchor_make(uint16_t file_id, SyntaxKind kind,
                                          uint32_t start, uint32_t length) {
    (void)kind; // kind was tracked on the legacy anchor for SyntaxNodePtr
                // rebind; FILE_RAW does today's byte-range path verbatim
                // and the rebind hint isn't carried. New emit sites that
                // need rebind use diag_anchor_file/body via the FileAstId
                // / BodyAstId maps instead.
    DiagAnchor a = {.kind = DIAG_ANCHOR_FILE_RAW, .file_id = file_id};
    a.u.raw.start = start;
    a.u.raw.length = length;
    return a;
}

// Capture a FILE_RAW diag anchor from a SyntaxNode. Backward-compatible
// helper for pre-Phase-P call sites.
static inline DiagAnchor diag_anchor_of_node(uint16_t file_id,
                                             const SyntaxNode *node) {
    SyntaxNodePtr ptr = syntax_node_ptr_new(node);
    return diag_anchor_make(file_id, ptr.kind, ptr.range.start,
                            ptr.range.length);
}

// New constructors for the structural anchor kinds (Phase P).
static inline DiagAnchor diag_anchor_file(uint16_t file_id, FileAstId id) {
    DiagAnchor a = {.kind = DIAG_ANCHOR_FILE, .file_id = file_id};
    a.u.file_ast_id = id;
    return a;
}

static inline DiagAnchor diag_anchor_body(DeclKey decl, RelAstId rel) {
    DiagAnchor a = {.kind = DIAG_ANCHOR_BODY};
    a.u.body.decl = decl;
    a.u.body.rel = rel;
    return a;
}

static inline bool diag_anchor_is_none(DiagAnchor a) {
    return a.kind == DIAG_ANCHOR_NONE_KIND;
}


// ---- ResolvedSpan ----------------------------------------------------
//
// A byte range translated into source-position context: file path,
// 1-indexed line + column, and the literal source line text. Output
// type for both the low-level db_resolve_span (TinySpan input) and
// the high-level DiagResolver (DiagAnchor input).
typedef struct {
  const char *path;
  uint32_t    line;          // 1-indexed
  uint32_t    col_start;     // 1-indexed
  uint32_t    col_end;       // 1-indexed, exclusive; clamped to the line
  const char *line_text;     // not NUL-terminated
  size_t      line_text_len;
} ResolvedSpan;


// ---- DiagResolver ----------------------------------------------------
//
// Single-slot LRU cache of one file's red root, threaded through a
// bulk diag render. Replaces the two-entry-point resolver API (the
// "one-off" / "with-root" split) — every caller now goes through the
// same path, and the cache amortizes the SyntaxTree + red-root build
// across all diags from one file before swapping out.
//
// Why slot-of-one: diag rendering is naturally file-clustered. LSP
// publish filters to one file at a time. CLI render iterates the
// result of db_collect_diags_for_file (also one file). Successive
// resolves within a file are 100% hits; on a file switch the prior
// root is released, the new one built. No HashMap needed.
//
// Lifetime: stack-allocate, init once before the loop, free once
// after. Init is cheap (no allocation). Free releases whatever is
// cached. Resolver does NOT outlive the request that observed the
// anchors — successive parses produce different green roots, and a
// stale cached root would point at a frozen-in-time tree (harmless
// memory-wise, but defeats the rebind purpose).
typedef struct {
    struct db   *db;
    uint16_t     cached_file_id;  // 0 = nothing cached
    SyntaxTree  *cached_tree;     // owned
    SyntaxNode  *cached_root;     // owned (+1 red root refcount)
} DiagResolver;

// Initialize a stack-allocated resolver. Cheap; no allocation.
void diag_resolver_init(DiagResolver *r, struct db *db);

// Release the currently-cached file root (if any). Idempotent — safe
// to call on a freshly-init'd resolver or twice in a row.
void diag_resolver_free(DiagResolver *r);

// Resolve a reparse-stable anchor against the current file tree.
// Lazily builds the file's red root on first observe of a new file_id,
// reusing the cached root for subsequent anchors in that file. Falls
// back to the captured (start, length) byte range when no matching
// node is found, when the file is unparsed, or when the file is
// evicted. Returns false when the file_id is 0 or the source has been
// evicted.
bool diag_resolver_resolve(DiagResolver *r, DiagAnchor anchor,
                           ResolvedSpan *out);


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
//   DIAG_ARG_SPAN   → printed as "file#N:start-end" (raw byte range;
//                     not resolved — primary anchor resolution is the
//                     DiagResolver's job; secondary spans stay cheap)
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

// 16 bytes total. Secondary spans carry a DiagAnchor (12 bytes,
// 4-byte aligned) — same reparse-stable encoding as the primary
// anchor — but render raw as "file#N:start-end" rather than going
// through the resolver, since related-info args aren't worth the
// per-arg ptr_resolve cost. All other union members are 4-byte
// aligned (StrId / IpIndex / int32 / uint32), so DiagAnchor's
// 4-byte alignment governs the union.
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

// Phase P: DiagArg grew from 16 B → 20 B because the SPAN variant
// embeds DiagAnchor, which grew from 12 → 16 B (tagged union).
// (The Phase P plan v2 predicted 24 B based on 4-byte enum kind, but
// DiagArgKind is `enum : uint8_t` so the layout is 1+3+16 = 20 B.)
// Bounded: typical diags carry 1–3 args, so ≤16 extra bytes/diag.
static_assert(sizeof(DiagArg) == 20, "DiagArg must stay 20 B (post-Phase-P)");


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

// N2 — LSP 3.17 DiagnosticTag (LSP §5.7). Editors render:
//   UNNECESSARY → faded / strike-through (unused decls, unreachable code)
//   DEPRECATED  → strike-through (use of deprecated API)
// Values match the LSP wire format exactly.
typedef enum {
    DIAG_TAG_NONE        = 0,
    DIAG_TAG_UNNECESSARY = 1,
    DIAG_TAG_DEPRECATED  = 2,
} DiagTag;

typedef struct {
    DiagAnchor     anchor;       // 12 — reparse-stable anchor
    StrId          template_id;  //  4 — interned template; resolves via db.strings
    const DiagArg *args;         //  8 — borrowed; points into slot's diag_arena
                                 //       NULL when n_args == 0
    uint16_t       code;         //  2 — diagnostic code (0 = uncategorized;
                                 //       future use: linkable docs, suppression keys)
    uint8_t        n_args;       //  1
    DiagSeverity   severity;     //  1
    uint8_t        tag;          //  1 — N2: DiagTag (0=NONE); fits in former pad
    uint8_t        owner_kind;   //  1 — O2: QueryKind of the owning slot,
                                 //       copied at emit time. Lets per-Diag
                                 //       consumers detect "parse error"
                                 //       (owner_kind == QUERY_FILE_AST) without
                                 //       a DiagList lookup. Sticky-squiggle gate
                                 //       in LSP publish_diagnostics needs it.
    uint8_t        _pad[2];      //  2 — alignment padding (was 3 before O2).
    // Total struct size 32 B unchanged — `owner_kind` consumed one byte of pad.
} Diag;

// Phase P: Diag grew from 32 B → 40 B (DiagAnchor 12 → 16, plus
// alignment pad to 8 B for the args pointer). One per cache line
// instead of two — acceptable for the architectural correctness win
// of structural anchors that survive reparse.
static_assert(sizeof(Diag) == 40, "Diag must stay 40 B (post-Phase-P)");


// ---- Phase P: DiagBundle / BundleColumn / DiagBundleRef / DiagSink ---
//
// (P7.1.0b — additive types; no call sites yet. Wired up in P7.1.2+.)
//
// Each emitting query owns a DiagBundle: a Vec<Diag> + its own Arena
// for DiagArg payloads. Bundles live INSIDE the query's result column
// (or in a parallel SoA column for kinds whose result is a bare
// IpIndex). Per-column arena ownership ensures BODY_SCOPES diags
// survive an INFER_BODY-only recompute (B2 fix from Phase P plan).

typedef struct {
    Vec   items;       // Vec<Diag>
    Arena args_arena;  // owns DiagArg payloads referenced by items[i].args
} DiagBundle;

// Enumerates every bundle-bearing column. Tagged into DiagBundleRef so
// the per-file index can route refs back to their owning SoA column at
// collect time. Each entry MUST have a corresponding _write hook AND a
// reclaim_slot dispatch entry (R6 cleanup discipline).
typedef enum {
    BUNDLE_COL_NONE = 0,
    BUNDLE_COL_FILE_PARSE_DIAGS,         // FILE_AST
    BUNDLE_COL_NS_SCOPES_DIAGS,          // NAMESPACE_SCOPES
    BUNDLE_COL_NS_TYPE_DIAGS,            // NAMESPACE_TYPE
    BUNDLE_COL_NS_CHECK_DIAGS,           // CHECK
    BUNDLE_COL_FN_SIG_DIAGS,             // FN_SIGNATURE
    BUNDLE_COL_FN_BODY_SCOPES_DIAGS,     // BODY_SCOPES (B2 — own column)
    BUNDLE_COL_FN_BODY_DIAGS,            // INFER_BODY  (B2 — own column)
    BUNDLE_COL_STRUCT_TYPE_DIAGS,        // TYPE_OF_DECL[KIND_STRUCT]
    BUNDLE_COL_VARIABLE_TYPE_DIAGS,      // TYPE_OF_DECL[KIND_VARIABLE]
    BUNDLE_COL_CONSTANT_TYPE_DIAGS,      // TYPE_OF_DECL[KIND_CONSTANT]
    BUNDLE_COL_UNION_TYPE_DIAGS,         // TYPE_OF_DECL[KIND_UNION]
    BUNDLE_COL_ENUM_TYPE_DIAGS,          // TYPE_OF_DECL[KIND_ENUM]
    BUNDLE_COL_EFFECT_TYPE_DIAGS,        // TYPE_OF_DECL[KIND_EFFECT]
    BUNDLE_COL_HANDLER_TYPE_DIAGS,       // TYPE_OF_DECL[KIND_HANDLER]
    BUNDLE_COLUMN_COUNT,
} BundleColumn;

// 8 B packed reference into the per-file bundle index. `col` selects
// the bundle-bearing column (SoA dispatch); `key` indexes the column
// (DefId.kind_row / NamespaceId.idx / FileId.idx — whichever is the
// column's primary key). bundle_deref(s, ref) routes col → SoA column.
typedef struct {
    uint16_t col;     // BundleColumn (only low 8 bits used today)
    uint16_t _pad;
    uint32_t key;
} DiagBundleRef;
static_assert(sizeof(DiagBundleRef) == 8, "DiagBundleRef must be 8 B");

// DiagSink — per-frame view of a bundle for the emit path. The query
// frame stack carries these; nested frames may inherit the parent's
// sink (pass-through queries with no own bundle) or push their own.
typedef struct {
    Vec           *items;       // -> DiagBundle.items
    Arena         *args_arena;  // -> DiagBundle.args_arena
    DiagBundleRef  ref;         // for per-file index bookkeeping
} DiagSink;

// Reset a bundle: vec_free + arena_free + re-zero. Idempotent (NULL ok).
// Phase P P2.b — called by every _write accessor that overwrites a
// bundle slot, by reclaim_slot's per-column dispatch, and by db_teardown.
void diag_bundle_free(DiagBundle *b);

// Per-file index removal — called from every reclaim_slot path for a
// bundle-bearing column BEFORE diag_bundle_free, while the slot is
// still authoritative for lookup. Phase P P2.a — R7 guard.
void diag_index_remove_ref(struct db *s, DiagBundleRef ref);

// Emit helpers — Phase P sink-routed. Will replace db_emit /
// db_emit_to / db_emit_tagged_to during the P7.1+ migration (legacy
// paths still available via ORE_DIAG_PURE_QUERIES feature flag).
// `s` is required for pool_intern on the message template + arena
// alloc on the args; callers always have it in scope.
void diag_sink_emit(struct db *s, DiagSink *sink, DiagSeverity severity,
                    DiagAnchor anchor, const char *fmt, ...);
void diag_sink_emit_tagged(struct db *s, DiagSink *sink, DiagSeverity severity,
                           DiagTag tag, DiagAnchor anchor,
                           const char *fmt, ...);


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
//   %P       DiagAnchor    — secondary location ("file#N:start-end")
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

// N2 — tagged emit. Like db_emit_to plus an LSP DiagnosticTag. Used by
// post-typecheck orchestration (sema's unused-decl walker) so the
// editor renders the identifier faded/strike-through instead of a
// full-strength squiggle. db_emit / db_emit_to leave tag = NONE.
void db_emit_tagged_to(struct db *s, QueryKind unit_kind, uint64_t unit_key,
                       DiagSeverity severity, DiagTag tag,
                       DiagAnchor anchor, const char *fmt, ...);


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

// Filtered collection — only diags whose primary.file matches `file`.
// Common pattern for LSP "publishDiagnostics" per-file messages.
void db_collect_diags_for_file(struct db *s, FileId file, Vec *out_diags);


// ---- Rendering -------------------------------------------------------
//
// Format one Diag into buf. snprintf-style return: number of bytes that
// WOULD have been written if buflen were unlimited; truncates to buflen
// and always NUL-terminates if buflen > 0.
//
// Pure template walker — substitutes {N} placeholders, never resolves
// anchor positions. (DIAG_ARG_SPAN args render as raw "file#N:start-end"
// previews; primary anchor → line:col resolution is the resolver's job.)
size_t db_format_diag(struct db *s, const Diag *d,
                      char *buf, size_t buflen);

// Streaming render — rust-style "path:line:col: severity: message" with
// source line + caret underline. Takes a DiagResolver so the per-file
// red-root build is amortized across all diags in a render loop. The
// resolver's slot-of-one LRU keeps file-clustered loops at one tree
// build per file, not per diag.
size_t diag_resolver_print(DiagResolver *r, const Diag *d, FILE *out);


// ---- Span resolution -------------------------------------------------
//
// Translate a TinySpan into its source-position context. Low-level
// primitive used internally by diag_resolver_resolve; exposed
// directly for the C26 test gate and any future consumer that
// already has a TinySpan in hand. Returns false when the span can't
// be resolved (virtual file with no on-disk path, file whose
// line_starts haven't been built, or evicted source). Callers fall
// back to whatever degenerate position their protocol requires.
bool db_resolve_span(struct db *s, TinySpan span, ResolvedSpan *out);


#endif // ORE_DB_DIAG_H
