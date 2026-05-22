#ifndef ORE_DB_H
#define ORE_DB_H

#include <assert.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "./ids/ids.h"
#include "./intern_pool/intern_pool.h"
#include "./query/query.h"
#include "./request/request.h"
#include "./storage/arena.h"
#include "./storage/hashmap.h"
#include "./storage/stringpool.h"
#include "./storage/vec.h"
#include "names.inc"

/* --- Column Types --- */

typedef enum : uint8_t {
  VIS_PRIVATE  = 0,
  VIS_PUBLIC   = 1,
  VIS_INTERNAL = 2,
} Visibility;

// Input durability — how often an input of this kind changes (Salsa
// tiers). Numeric value = volatility rank: LOW is the most volatile
// (workspace file text, edited constantly), HIGH the most stable
// (library sources). A derived query's durability is the MIN (most
// volatile) over its inputs, so "min" == numeric min. Used purely to
// make early-cutoff cheaper: a query provably free of any input at-or-
// below its durability can skip the dependency walk entirely.
typedef enum : uint8_t {
  DUR_LOW    = 0, // workspace file text — changes on every keystroke
  DUR_MEDIUM = 1, // file->module map / module metadata
  DUR_HIGH   = 2, // library sources — effectively immutable in a session
} Durability;
#define DUR_COUNT 3

typedef enum : uint8_t {
  SCOPE_NONE = 0,
  SCOPE_MODULE,
  SCOPE_FUNCTION,
  SCOPE_BLOCK,
  SCOPE_STRUCT,
  SCOPE_UNION,
  SCOPE_ENUM,
} ScopeKind;

typedef struct {
  StrId name;
  AstId ast_id; // Stable parser-side identity. Consumers route through
                // db_query_def_identity(mid, ast_id) to materialize the
                // canonical DefId on demand — the slot+DefId live in
                // db.def_by_identity (HashMap) and persist across
                // db_query_module_exports re-runs, so name/decl edits
                // never re-allocate the DefId for an unchanged AstId.
                //
                // Deliberately no `def` field: keeping the DefId off
                // DeclEntry structurally prevents callers from caching
                // and reading a stale DefId across re-runs.
} DeclEntry;

// TinySpan — 8-byte packed source byte range.
//
// Field layout:
//   bits  0-15: file_id  (16 bits, ~65k files max)
//   bits 16-39: start    (24 bits, 16MB byte offset max)
//   bits 40-63: length   (24 bits, 16MB token length max — see assert in
//                          db_create_source)
//
// Implemented as a plain uint64_t + inline accessors
typedef uint64_t TinySpan;

#define TINYSPAN_NONE ((TinySpan)0)

#define TINYSPAN_FILE_MASK    ((TinySpan)0xFFFFu)
#define TINYSPAN_OFFSET_MASK  ((TinySpan)0xFFFFFFu)
#define TINYSPAN_START_SHIFT  16
#define TINYSPAN_LENGTH_SHIFT 40

static inline uint16_t span_file(TinySpan s) {
    return (uint16_t)(s & TINYSPAN_FILE_MASK);
}
static inline uint32_t span_start(TinySpan s) {
    return (uint32_t)((s >> TINYSPAN_START_SHIFT) & TINYSPAN_OFFSET_MASK);
}
static inline uint32_t span_length(TinySpan s) {
    return (uint32_t)((s >> TINYSPAN_LENGTH_SHIFT) & TINYSPAN_OFFSET_MASK);
}
static inline uint32_t span_end(TinySpan s) {
    return span_start(s) + span_length(s);
}

static inline TinySpan span_make(uint16_t file, uint32_t start, uint32_t length) {
    // Bounds: file ≤ 16 bits (already enforced by the param type),
    // start + length ≤ 24 bits each (16MB position / 16MB length). Without
    // the assert, oversized values silently mask off high bits into the
    // file_id space, corrupting spans cross-file. Asserts compile out
    // under NDEBUG; the trailing `& MASK` then keeps the bit-overflow
    // contained even in release.
    assert(start  <= (uint32_t)TINYSPAN_OFFSET_MASK && "span start  > 24 bits — file > 16 MB?");
    assert(length <= (uint32_t)TINYSPAN_OFFSET_MASK && "span length > 24 bits — token > 16 MB?");
    return ((TinySpan)file & TINYSPAN_FILE_MASK)
         | (((TinySpan)start  & TINYSPAN_OFFSET_MASK) << TINYSPAN_START_SHIFT)
         | (((TinySpan)length & TINYSPAN_OFFSET_MASK) << TINYSPAN_LENGTH_SHIFT);
}

// Construct from (start, end-exclusive) — the common parser pattern.
static inline TinySpan span_make_range(uint16_t file, uint32_t start, uint32_t end) {
    return span_make(file, start, end - start);
}

// Replace the file_id of an existing span, preserving start + length.
static inline TinySpan span_with_file(TinySpan s, uint16_t file) {
    return (s & ~TINYSPAN_FILE_MASK) | ((TinySpan)file & TINYSPAN_FILE_MASK);
}

typedef struct {
#define X(id, name) StrId id;
    PRIMITIVE_LIST(X)
    BUILTIN_LIST(X)
    FIELD_LIST(X)
    CONTEXT_LIST(X)
#undef X
} DbNames;

typedef struct {
  StrId path;
  DefId def;
  struct QuerySlot slot;
} ResolvePathEntry;

// def_identity and resolve_ref no longer use embedded-slot entry structs:
// their slots live in dense db.def_identity / db.resolve_ref SoA columns,
// routed by the db.def_by_identity / db.resolve_ref_cache HashMaps.

// --- Centralized diagnostics --------------------------------------------
//
// Diagnostics are keyed by ANALYSIS UNIT — the (QueryKind, key) pair of the
// query that produced them — in db.diags (a HashMap<u64, DiagList*>). The u64
// key packs (QueryKind << 56) | key_bits; see src/db/diag/diag.h. One DiagList
// is lazily created per unit that emits at least one diag, so the map stays
// O(units-with-diags) and per-file collection is O(emitted diags) rather than
// O(all slots).
//
// A DiagList owns its diags' backing memory: `items` is a Vec<Diag> and
// `arena` holds the byte-copied DiagArg payloads. The DiagList struct itself
// is arena-allocated in db.arena (pointer-stable); db_diags_clear resets it
// when the producing query recomputes.
typedef struct {
  Vec   items;   // Vec<Diag>
  Arena arena;   // owns the Diag args byte-copies
} DiagList;

// --- Body scope tree (rust-analyzer ExprScopes, adapted to AstNodeId) ---
//
// Per-fn lexical scope structure. Built by db_query_body_scopes (sema-
// side impl in sema/body_scopes.c). The data is stored flat in three
// shared db pools; db.fns.body[row] holds the per-fn (off,len) ranges
// into them (see FnBody below):
//
//   db.body_scope_rows  — ScopeRow. Each row has a parent (the tree).
//                         scope 0 = fn-root (holds params).
//   db.body_scope_binds — ScopedBind. Each bind tags its owning scope_id
//                         so multiple scopes can interleave bind pushes.
//   db.node_to_scope    — uint32_t. Sized to the body's AstNodeId span,
//                         indexed by (node.idx - body_root_min) → scope.
//
// Lookup: scope = n2s[use_node - body_root_min]; scan binds for matches
// in that scope (latest wins for shadows); on miss, walk to parent
// scope; repeat. Cost: O(depth × binds_count). Bodies are small.
#define BODY_SCOPE_NONE ((uint32_t)0xFFFFFFFF)

typedef struct {
  uint32_t  parent;      // BODY_SCOPE_NONE for the root
  AstNodeId block_node;  // AST node that opened this scope (debug + LSP)
} ScopeRow;

typedef struct {
  uint32_t scope_id;     // which ScopeRow this bind belongs to
  StrId    name;
  IpIndex  type;
} ScopedBind;

// Per-fn body-scope ranges, stored in db.fns.body[row]. The (off,len)
// pairs slice the three body-scope pools; body_root_min is the body's
// smallest AstNodeId.idx (db.node_to_scope is indexed relative to it).
typedef struct {
  uint32_t scope_off, scope_len;   // -> db.body_scope_rows
  uint32_t bind_off,  bind_len;    // -> db.body_scope_binds
  uint32_t n2s_off,   n2s_len;     // -> db.node_to_scope
  uint32_t body_root_min;
} FnBody;

// A per-file flat array, stored in that file's arena (files.arenas[file]).
// Replaces the former Vec<Vec<...>> per-file columns (line_starts, trivia):
// reparse resets the per-file arena, so the array is rebuilt with no
// separate malloc and no Vec-of-Vec indirection — and with zero dead slack
// (a global pool would strand the prior parse's slice on every edit).
// `data`'s element type is column-specific — uint32_t for line_starts and
// trivia_offsets, TriviaSpan for trivia_tokens.
typedef struct {
  void    *data;
  uint32_t count;
} FileArray;

// Per-file node-side data — a single allocation per file containing
// three parallel arrays indexed by AstNodeId. The pointers are interior
// to one contiguous block so spans/parents/types for a given file
// stay cache-coherent on AST walks. The element count for all three is
// files.node_counts[file_id].
typedef struct {
  TinySpan  *spans;    // [node_counts[mod_id]]
  AstNodeId *parents;  // [node_counts[mod_id]]
  IpIndex   *types;    // [node_counts[mod_id]]
  // node_to_def: per-AST-node reverse index, populated by
  // db_query_module_exports. defs[N] is DEF_ID_NONE unless N is the
  // AstNodeId of a top-level decl, in which case it's that decl's
  // DefId. db_get_def_for_node walks up `parents` until it finds a
  // non-NONE entry — that's the enclosing top-level decl.
  DefId     *defs;     // [node_counts[mod_id]]
} ModuleNodeData;

/* --- Definition Metadata (8 bits) --- */
typedef uint8_t DefMeta;
#define META_VIS_MASK 0x03 // Bits 0-1 (Visibility)
#define META_COMPTIME 0x04 // Bit 2 (is_comptime)
#define META_SCOPED 0x08   // Bit 3 (scoped effect)
#define META_NAMED 0x10    // Bit 4 (named effect/handler)
#define META_LINEAR 0x20   // Bit 5 (linear effect/handler)
#define META_ABSTRACT 0X40 // Bit 6 (public construct / private fields)
#define META_DISTINCT 0x80 // Bit 7 (distinct constructs like ints, fns)

typedef struct {
  StrId name;
  AstNodeId node;
  DefMeta meta;
  AstId ast_id;
} TopLevelEntry;


/* --- Scope Metadata (8 bits) --- */
typedef uint8_t ScopeMeta;
#define META_SCOPE_KIND_MASK 0x07 // Bits 0-2 (ScopeKind)

/* --- The Database --- */
//
// CONCURRENCY CONTRACT
// ────────────────────
// The atomics on this struct (rev_control, dur_last_changed,
// cancel_requested) DO NOT mean queries run on a thread pool. They
// support a specific pattern: single-mutator query execution with
// concurrent cross-thread *signals* into the db.
//
// Allowed concurrent access:
//   • Reader threads — none today, but query result-reads (post-
//     succeed) are thread-safe via the atomics.
//   • Signaller threads (LSP edit notifier, cancel button) call
//     db_input_changed / db_request_cancel from any thread to bump
//     rev_control / set cancel_requested. The query thread observes
//     these atomically on its next check.
//
// NOT allowed:
//   • Concurrent query execution. All Vec pushes, HashMap inserts,
//     and intern-pool grows happen single-threaded inside query
//     bodies. The SoA columns + HashMap caches are deliberately
//     unguarded because we never race writers.
//
// To go to a parallel query scheduler in the future we'd need either
// per-table rwlocks, concurrent containers, or rust-analyzer's "one
// salsa runtime per thread, share inputs only" pattern. None of that
// is wired today.
struct db {
  /* --- Control & Invalidation ---  */
  // [1 bit: Invalidation] [ 31 bits: Current Rev ] [ 32 bits: Request Rev ]
  _Alignas(64) _Atomic uint64_t rev_control;

// Bitmasks
#define REV_INVALIDATION_MASK (1ULL << 63)
#define REV_CURRENT_MASK (0x7FFFFFFFULL << 32)
#define REV_REQUEST_MASK (0xFFFFFFFFULL)

  // Per-durability "last changed" revision. dur_last_changed[D] = the
  // global revision at which an input of durability D last changed. An
  // edit bumps the global revision AND dur_last_changed[edited input's
  // durability]. db_revalidate early-cuts a slot when no input at its
  // durability tier changed since it was last verified — without
  // walking its dependency graph at all. Initialized to the db's
  // starting revision (1) so the first revalidation is exact.
  _Atomic uint64_t dur_last_changed[DUR_COUNT];

  uint32_t comptime_depth_limit;

  /* --- Cancellation --- */
  _Alignas(64) atomic_bool cancel_requested;

  /* --- WARM: Global Managers -------------------------------------------- */
  _Alignas(64) Arena arena;
  Arena request_arena;
  StringPool strings;
  InternPool intern;
  Vec query_stack; // Vec<QueryFrame>

  // Pre-interned StrIds for hot builtin + contextual-keyword identifiers.
  // Populated at db_init from BUILTIN_LIST / CONTEXT_LIST (names.inc).
  DbNames names;

  /* --- COLD: The Tables (SoA Headers) ----------------------------------- */

  // FILES — the per-file parse unit. One row per FileId; the parse
  // query (QUERY_FILE_AST) is keyed here. Every column is file-scoped
  // (one file's parse artifacts), so editing one file reparses only
  // its row. source_id / module_id are explicit back-refs that keep
  // source, file, and module distinct id spaces (1:1 today, N:1 ready).
  //
  // Fully rowed — every column grows one zero row per FileId in lockstep
  // (db_create_file). X-macro driven init / push_zero / free so a new
  // column can't be added to one of the three and forgotten in another.
  //
  //   ids               — the row's own FileId; pointer-stable
  //                        QUERY_FILE_AST slot key (db_locate_slot derefs).
  //   source_id         — backing source (text/version/hash).
  //   module_id         — owning module (back-ref).
  //   line_starts       — FileArray of uint32_t[] line-start byte offsets
  //                        in files.arenas[f]; built by the lexer, consumed
  //                        by diagnostic / LSP-position derivation.
  //   node_data         — ModuleNodeData: a contiguous block of TinySpan[],
  //                        AstNodeId[], IpIndex[], DefId[] all sized to
  //                        node_counts[f] (see the ModuleNodeData typedef).
  //   node_counts       — node count for this file's ModuleNodeData arrays.
  //   arenas            — per-file durable arena: hosts the ASTStore, the
  //                        flattened node-data block, and the FileArray
  //                        bodies. arena_reset at the top of QUERY_FILE_AST
  //                        before a reparse (O(1) per-file isolation),
  //                        arena_free in db_ids_free.
  //   asts              — struct ASTStore *.
  //   trivia_tokens     — FileArray of TriviaSpan[] in files.arenas[f].
  //   trivia_offsets    — FileArray of uint32_t[] in files.arenas[f]
  //                        (parallel to the real-token stream, +1 sentinel).
  //   ast_id_maps       — struct AstIdMap *.
  //   top_level_indices — FileArray of TopLevelEntry[] in files.arenas[f].
  //   slots_ast         — per-file QUERY_FILE_AST slot. Dense SoA column so
  //                        the revalidation walker iterates one kind
  //                        densely. Slot pointers are NOT cached by callers
  //                        — db_locate_slot re-resolves on every
  //                        db_query_begin via (kind, FileId).
#define ORE_FILES_COLUMNS(X)                \
    X(ids,               FileId)            \
    X(source_id,         SourceId)          \
    X(module_id,         ModuleId)          \
    X(line_starts,       FileArray)         \
    X(node_data,         ModuleNodeData)    \
    X(node_counts,       uint32_t)          \
    X(arenas,            Arena)             \
    X(asts,              void *)            \
    X(trivia_tokens,     FileArray)         \
    X(trivia_offsets,    FileArray)         \
    X(ast_id_maps,       void *)            \
    X(top_level_indices, FileArray)         \
    X(slots_ast,         struct QuerySlot)
  struct {
#define X(name, type) Vec name;
    ORE_FILES_COLUMNS(X)
#undef X
  } files;

  // MODULES — a thin aggregation over a set of files. Holds only
  // genuinely module-scoped state; its contents (top-level index /
  // exports) are DERIVED queries that aggregate over the module's
  // files (each recording a dep on that file's QUERY_FILE_AST, so an
  // edit to one file early-cuts the others).
  //
  // Plain rowed columns are X-macro driven (init / push_zero / free from
  // one list). The file_offsets/file_pool flat-pool PAIR is deliberately
  // NOT in the X-macro — it is not a plain rowed column (see below).
  //
  //   ids             — pointer-stable module-query slot key.
  //   names           — Vec<StrId>.
  //   exports /        Lazily-allocated per-module scope ids, SCOPE_ID_NONE
  //   internal_scopes  until db_query_module_exports first runs. Internal
  //                    scope collects every decl; export scope mirrors only
  //                    the public subset (importers query export to stay
  //                    stable across private-decl edits).
  //   slots_index /    QUERY_TOP_LEVEL_INDEX / QUERY_MODULE_EXPORTS slots.
  //   slots_exports
#define ORE_MODULES_COLUMNS(X)              \
    X(ids,             ModuleId)            \
    X(names,           StrId)               \
    X(exports,         ScopeId)             \
    X(internal_scopes, ScopeId)             \
    X(slots_index,     struct QuerySlot)    \
    X(slots_exports,   struct QuerySlot)
  struct {
#define X(name, type) Vec name;
    ORE_MODULES_COLUMNS(X)
#undef X
    // Flat-pool pair — NOT a plain rowed column, so kept out of
    // ORE_MODULES_COLUMNS and hand-initialized in ids.c. The module's
    // file list is a flat pool + per-module [start,end) offsets (same
    // idiom as scopes.decl_offsets/decl_pool): module M's files =
    // file_pool[file_offsets[M] .. file_offsets[M+1]). file_offsets is
    // seeded count == module_count + 1; file_pool starts empty.
    Vec file_offsets; // Vec<uint32_t> — count == module_count + 1
    Vec file_pool;    // Vec<FileId>
  } modules;

  // --- Definition tables ------------------------------------------------
  //
  // db.defs is a THIN shared SoA: the identity every kind needs, indexed
  // directly by DefId. It carries no per-kind state — kinds[d] selects
  // one of the eight per-kind tables below, kind_row[d] indexes into it.
  //
  // Each per-kind table is itself a SoA (parallel dense columns): every
  // column holds only that kind's data, so the revalidation walker over
  // one slot column, and any by-kind bulk query, both touch dense memory
  // with zero sparse entries.
  //
  // db.defs grows in lockstep via db_create_def; each per-kind table
  // grows in lockstep via db_def_set_kind. The X-macros make "can't
  // forget a column" mechanical — init / push_zero / free expand from
  // one list. The X-macros are NOT #undef'd: ids.c expands them too.
  //
  // The DEF_IDENTITY slot is NOT here — it lives in db.def_identity.slots
  // (routed by db.def_by_identity), so DefIds keep canonical stable
  // identity across module_exports re-runs.
#define ORE_DEFS_COLUMNS(X) \
    X(names,          StrId)    \
    X(ast_ids,        AstId)    \
    X(parent_modules, ModuleId) \
    X(meta,           DefMeta)  \
    X(kinds,          DefKind)  \
    X(kind_row,       uint32_t)
  struct {
#define X(name, type) Vec name;
    ORE_DEFS_COLUMNS(X)
#undef X
  } defs;

  // FUNCTION — db.fns. type/signature are cached query outputs (a fn's
  // type_of_def delegates to fn_signature, so both hold the fn type);
  // body holds the per-fn body-scope ranges into the pools below.
#define ORE_FNS_COLUMNS(X) \
    X(type,             IpIndex)          \
    X(signature,        IpIndex)          \
    X(slot_type,        struct QuerySlot) \
    X(slot_signature,   struct QuerySlot) \
    X(slot_infer,       struct QuerySlot) \
    X(slot_body_scopes, struct QuerySlot) \
    X(body,             FnBody)
  struct {
#define X(name, type) Vec name;
    ORE_FNS_COLUMNS(X)
#undef X
  } fns;

  // STRUCT / UNION / ENUM / EFFECT / HANDLER / VARIABLE — uniform shape:
  // a cached QUERY_TYPE_OF_DECL result + its slot. Kept as distinct
  // tables (one per DefKind) so a by-kind scan stays dense. Field /
  // variant / param payloads are NOT duplicated here — they live in the
  // InternPool as part of the interned struct / enum / fn type.
#define ORE_STRUCTS_COLUMNS(X) \
    X(type,      IpIndex)          \
    X(slot_type, struct QuerySlot)
  struct {
#define X(name, type) Vec name;
    ORE_STRUCTS_COLUMNS(X)
#undef X
  } structs;

#define ORE_UNIONS_COLUMNS(X) \
    X(type,      IpIndex)          \
    X(slot_type, struct QuerySlot)
  struct {
#define X(name, type) Vec name;
    ORE_UNIONS_COLUMNS(X)
#undef X
  } unions;

#define ORE_ENUMS_COLUMNS(X) \
    X(type,      IpIndex)          \
    X(slot_type, struct QuerySlot)
  struct {
#define X(name, type) Vec name;
    ORE_ENUMS_COLUMNS(X)
#undef X
  } enums;

#define ORE_EFFECTS_COLUMNS(X) \
    X(type,      IpIndex)          \
    X(slot_type, struct QuerySlot)
  struct {
#define X(name, type) Vec name;
    ORE_EFFECTS_COLUMNS(X)
#undef X
  } effects;

#define ORE_HANDLERS_COLUMNS(X) \
    X(type,      IpIndex)          \
    X(slot_type, struct QuerySlot)
  struct {
#define X(name, type) Vec name;
    ORE_HANDLERS_COLUMNS(X)
#undef X
  } handlers;

#define ORE_VARIABLES_COLUMNS(X) \
    X(type,      IpIndex)          \
    X(slot_type, struct QuerySlot)
  struct {
#define X(name, type) Vec name;
    ORE_VARIABLES_COLUMNS(X)
#undef X
  } variables;

  // CONSTANT — db.constants. Adds slot_const_eval for the (declared,
  // not-yet-implemented) QUERY_CONST_EVAL.
#define ORE_CONSTANTS_COLUMNS(X) \
    X(type,            IpIndex)          \
    X(slot_type,       struct QuerySlot) \
    X(slot_const_eval, struct QuerySlot)
  struct {
#define X(name, type) Vec name;
    ORE_CONSTANTS_COLUMNS(X)
#undef X
  } constants;

  // HASHMAP-KEYED QUERIES — def_identity and resolve_ref. Their slots
  // live in dense Vec<QuerySlot> columns (like the per-kind tables);
  // db.def_by_identity / db.resolve_ref_cache route a packed u64 key to
  // a row index here. Row 0 of each is a reserved sentinel; `results`
  // holds the per-row cached DefId.
  struct {
    Vec results;  // Vec<DefId>
    Vec slots;    // Vec<QuerySlot>
  } def_identity;
  struct {
    Vec results;  // Vec<DefId>
    Vec slots;    // Vec<QuerySlot>
  } resolve_ref;

  // Body-scope pools. db.fns.body[row] holds per-fn (off,len) ranges
  // into these three flat arrays (rust-analyzer ExprScopes, flattened).
  Vec body_scope_rows;   // Vec<ScopeRow>
  Vec body_scope_binds;  // Vec<ScopedBind>
  Vec node_to_scope;     // Vec<uint32_t>


  // SCOPES — plain rowed columns are X-macro driven; the decl_offsets/
  // decl_pool flat-pool pair is hand-managed (see modules.file_offsets).
  // (Per-(scope, name) name-resolution slots live in db.resolve_ref,
  //  routed by db.resolve_ref_cache — many names per scope.)
#define ORE_SCOPES_COLUMNS(X)               \
    X(parents,        ScopeId)              \
    X(meta,           ScopeMeta)            \
    X(owning_modules, ModuleId)
  struct {
#define X(name, type) Vec name;
    ORE_SCOPES_COLUMNS(X)
#undef X
    // Flat-pool pair — NOT plain rowed columns (see modules.file_offsets).
    // decl_offsets is seeded count == scope_count + 1; decl_pool empty.
    Vec decl_offsets;      // Vec<uint32_t>
    Vec decl_pool;         // Vec<DeclEntry>
  } scopes;

  // SOURCES — raw source text + metadata, one row per SourceId. Fully
  // rowed; X-macro driven init / push_zero / free.
#define ORE_SOURCES_COLUMNS(X)              \
    X(hashes,     uint64_t)                 \
    X(versions,   uint32_t)                 \
    X(paths,      StrId)                    \
    X(texts,      char *)                   \
    X(text_lens,  uint32_t)                 \
    X(durability, Durability)
  struct {
#define X(name, type) Vec name;
    ORE_SOURCES_COLUMNS(X)
#undef X
  } sources;

  /* --- COLDER: Sparse Caches (HashMaps) --------------------------------- */
  HashMap module_by_path;
  // path_id (StrId.idx, u32 promoted to u64) → SourceId.idx (u32).
  // Populated by db_create_source; read by db_lookup_source_by_path for
  // O(1) lookup. Monotone — entries are added but never removed
  // (sources are append-only for the db's lifetime). Salsa doesn't
  // need to dep-track this lookup; it's pure structural mapping.
  HashMap source_by_path;
  // SourceId.idx → FileId.idx. Populated by db_create_file. 1:1 today
  // (one file per source); when N:1 lands the value becomes an offset
  // into a side Vec<FileId> of files-per-source. Same rationale as
  // source_by_path: pure structural reverse index, no salsa needed.
  HashMap file_by_source;
  HashMap resolve_path;
  HashMap def_by_identity;     // (mid.idx<<32 | ast_id.idx) → db.def_identity row
  HashMap resolve_ref_cache;   // (scope.idx<<32 | name.idx) → db.resolve_ref row
  HashMap comptime_call_cache;

  // Centralized diagnostics — (QueryKind, key) analysis unit → DiagList*.
  // Replaces the former per-QuerySlot diag_arena. Keyed by the packed u64
  // from db_diag_unit_key; see the DiagList typedef above and
  // src/db/diag/diag.h. Values are arena-allocated in db.arena.
  HashMap diags;
};

void db_init(struct db *s);
void db_free(struct db *s);

// --- Per-DefId routing -------------------------------------------------
//
// kinds[d] selects the per-kind table; kind_row[d] indexes every column
// of it. All per-kind column access (db.fns.*, db.structs.*, …) routes
// through db_def_row, which asserts the def is classified to `want`.
static inline DefKind db_def_kind(struct db *s, DefId d) {
  return *(DefKind *)vec_get(&s->defs.kinds, d.idx);
}
static inline uint32_t db_def_row(struct db *s, DefId d, DefKind want) {
  assert(db_def_kind(s, d) == want && "db_def_row: def kind mismatch");
  (void)want;
  return *(uint32_t *)vec_get(&s->defs.kind_row, d.idx);
}

// The QUERY_TYPE_OF_DECL result cell for a def — its per-kind table's
// `type` column. Every kind has one. The pointer is into a Vec buffer:
// do NOT hold it across a call that can grow a per-kind table (re-fetch
// after, as the query wrappers do).
static inline IpIndex *db_def_type_cell(struct db *s, DefId d) {
  uint32_t row = *(uint32_t *)vec_get(&s->defs.kind_row, d.idx);
  switch (db_def_kind(s, d)) {
  case KIND_FUNCTION: return (IpIndex *)vec_get(&s->fns.type, row);
  case KIND_STRUCT:   return (IpIndex *)vec_get(&s->structs.type, row);
  case KIND_UNION:    return (IpIndex *)vec_get(&s->unions.type, row);
  case KIND_ENUM:     return (IpIndex *)vec_get(&s->enums.type, row);
  case KIND_EFFECT:   return (IpIndex *)vec_get(&s->effects.type, row);
  case KIND_HANDLER:  return (IpIndex *)vec_get(&s->handlers.type, row);
  case KIND_VARIABLE: return (IpIndex *)vec_get(&s->variables.type, row);
  case KIND_CONSTANT: return (IpIndex *)vec_get(&s->constants.type, row);
  default: break;
  }
  assert(0 && "db_def_type_cell: unclassified def");
  return NULL;
}

// The QUERY_FN_SIGNATURE result cell — db.fns.signature. Asserts the
// def is a function. Same pointer-stability caveat as db_def_type_cell.
static inline IpIndex *db_fn_signature_cell(struct db *s, DefId d) {
  return (IpIndex *)vec_get(&s->fns.signature,
                            db_def_row(s, d, KIND_FUNCTION));
}

// =============================================================================
//                          INPUT BOUNDARY
//
// Every public mutation of db state goes through one of the functions
// below. Outside src/db/, nothing should write to the SoA columns
// directly. Pattern:
//
//   db_create_<entity>   — allocate a new id (Source / File / Module / …)
//   db_set_<entity>_*    — update a field on an existing id
//   db_add_*_to_*        — link two ids (file → module)
//   db_emit_<severity>   — push a Diag into the active query slot
//
// Bodies live in src/db/setters/<entity>.c. The query engine writes
// query results internally; that is NOT the input boundary.
// =============================================================================

// --- Setters: source ---------------------------------------------------------
SourceId db_create_source(struct db *s, const char *path, size_t path_len,
                          const char *text, size_t text_len);
bool     db_set_source_text(struct db *s, SourceId src,
                            const char *text, size_t text_len);
void     db_set_source_durability(struct db *s, SourceId src, uint8_t dur);

// --- Setters: file -----------------------------------------------------------
FileId   db_create_file(struct db *s, SourceId src, ModuleId owner);

// --- Setters: module ---------------------------------------------------------
ModuleId db_create_module(struct db *s);
void     db_add_file_to_module(struct db *s, ModuleId mid, FileId fid);

// --- Setters: diag (emit into the active query slot) -------------------------
//   Declared in src/db/diag/diag.h (db_emit_error, _warning, _info, _hint,
//   plus typed variants _c / _s / _n / _t / _ss / _va).
//   The header lives with the Diag/DiagSeverity types; the bodies live in
//   src/db/setters/diag.c.

// =============================================================================
//                          READ BOUNDARY
//
//   db_get_<entity>_<field>   — direct column read (cheap, no caching)
//   db_lookup_<entity>_by_<k> — structural lookup (HashMap or scan)
//   db_collect_*              — slot walks (e.g. per-file diags)
//   db_format_* / db_print_*  — string renderers
//   db_resolve_*              — derived helpers (span → file/line/col, etc.)
//
// Bodies live in src/db/getters/<entity>.c.
// =============================================================================

// --- Getters: source ---------------------------------------------------------
const char *db_get_source_text     (struct db *s, SourceId src);
uint32_t    db_get_source_len      (struct db *s, SourceId src);
StrId       db_get_source_path     (struct db *s, SourceId src);
uint32_t    db_get_source_version  (struct db *s, SourceId src);
Durability  db_get_source_durability(struct db *s, SourceId src);
SourceId    db_lookup_source_by_path(struct db *s, const char *path,
                                     size_t path_len);

// --- Getters: file -----------------------------------------------------------
SourceId db_get_file_source      (struct db *s, FileId fid);
ModuleId db_get_file_module      (struct db *s, FileId fid);
FileId   db_lookup_file_by_source(struct db *s, SourceId src);
struct ASTStore   *db_get_file_ast        (struct db *s, FileId fid);
struct AstIdMap   *db_get_file_ast_id_map (struct db *s, FileId fid);
TinySpan db_get_node_span(struct db *s, FileId fid, AstNodeId node);
// Enclosing top-level DefId for an AST node. Walks the parent chain
// up to the nearest decl-root (stamped by db_query_module_exports
// into ModuleNodeData.defs). DEF_ID_NONE if the node isn't inside
// any top-level decl, or module_exports hasn't run for this file.
DefId    db_get_def_for_node(struct db *s, FileId fid, AstNodeId node);

// --- Getters: module ---------------------------------------------------------
const FileId *db_get_module_files(struct db *s, ModuleId mid,
                                  uint32_t *out_count);

// --- Getters: diag -----------------------------------------------------------
//   db_collect_diags_all, db_collect_diags_for_file, db_format_diag,
//   db_print_diag, db_resolve_span — declared in src/db/diag/diag.h
//   alongside the Diag/ResolvedSpan types.

// --- Getters: type rendering -------------------------------------------------
size_t db_format_type(struct db *s, IpIndex t, char *buf, size_t cap);

// --- Getters: position / cursor (LSP, CLI --at-position) ---------------------
uint32_t  db_byte_offset_at(struct db *s, FileId fid,
                            uint32_t line0, uint32_t char0);
AstNodeId db_node_at_offset(struct db *s, FileId fid, uint32_t byte_offset);

#endif
