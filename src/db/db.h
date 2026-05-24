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

// Per-file artifacts owned elsewhere (parser / workspace) — db.files
// stores typed pointers to them, so forward-declare the tags here.
struct ASTStore;
struct AstIdMap;

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
                // db_query_def_identity(nsid, ast_id) to materialize the
                // canonical DefId on demand — the slot+DefId live in
                // db.def_by_identity (HashMap) and persist across
                // db_query_namespace_scopes re-runs, so name/decl edits
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
//
// Defensive clamp: if `end < start` (parser recovery left the cursor at
// or before the captured start anchor) the subtraction would underflow
// to a huge u32, which span_make's length-bounds assert would then
// flag as a SIGABRT. In debug builds we keep that assert loud so the
// underlying parser bug is caught at the call site. In release builds
// the clamp turns it into TINYSPAN_NONE (a degenerate span at file=0
// byte=0) — diags / hover degrade to "line 1:1" but the process stays
// up. The proper fix lives at the call site; this is belt + suspenders.
static inline TinySpan span_make_range(uint16_t file, uint32_t start, uint32_t end) {
    assert(end >= start && "span_make_range: end < start (parser cursor bug)");
    if (end < start) return TINYSPAN_NONE;
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

// def_identity, resolve_ref and resolve_path do not use embedded-slot
// entry structs: their slots live in dense db.def_identity /
// db.resolve_ref / db.resolve_path SoA columns, routed by the
// db.def_by_identity / db.resolve_ref_cache / db.resolve_path_cache
// HashMaps.

// --- Centralized diagnostics --------------------------------------------
//
// Diagnostics are keyed by ANALYSIS UNIT — the (QueryKind, key) pair of the
// query that produced them. DiagLists live in the dense db.diag_lists Vec;
// db.diags routes a unit key (QueryKind << 56) | key_bits to a row. One row
// is lazily appended per unit that emits at least one diag, so storage stays
// O(units-with-diags) and collection is O(emitted diags) rather than
// O(all slots). See src/db/diag/diag.h.
//
// A DiagList owns its diags' backing memory: `items` is a Vec<Diag> and
// `arena` holds the byte-copied DiagArg payloads. The struct lives by value
// in db.diag_lists and is relocatable — its Arena and Vec are relocatable
// structs, and the heap they own does not move on a diag_lists realloc.
// db_diags_clear resets it when the producing query recomputes.
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
  // node_to_def: per-AST-node reverse index, populated by
  // QUERY_NODE_TO_DECL. defs[N] is DEF_ID_NONE unless N is the
  // AstNodeId of a top-level decl, in which case it's that decl's
  // DefId. db_get_def_for_node walks up `parents` until it finds a
  // non-NONE entry — that's the enclosing top-level decl.
  DefId     *defs;     // [node_counts[mod_id]]
  // Per-AST-node type cache. Populated by sema_type_of_expr (wrapper
  // writes its return value to types[node.idx]). Used by:
  //   - Phase 7 infer_body's typed-body fingerprint sweep.
  //   - (future) LSP hover for arbitrary expression types.
  //   - (future) codegen reading per-node types when emitting MIR.
  // Memset-zero initialized at parse; nodes never visited by sema
  // stay at IpIndex{0} (the first reserved pool entry — a valid but
  // meaningless type for fingerprint purposes; stable across reparses).
  IpIndex   *types;    // [node_counts[mod_id]]
} FileNodeData;

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

// Per-namespace (per-file) scope record, one per NamespaceId in
// db.namespaces.exports.
//
//   internal   — every top-level decl in scope; used to resolve bare
//                identifiers WITHIN this file. SCOPE_ID_NONE until
//                db_query_namespace_scopes first runs.
//   exported   — legacy: the public export scope. Subsumed by
//                struct_type post-Phase-2; kept until Phase 2h cleanup
//                so the export-scope-based path stays buildable until
//                all consumers are switched over.
//   struct_type — the namespace-as-struct-type IpIndex (IPK_NAMESPACE_TYPE).
//                Built lazily by db_query_namespace_type; field set =
//                public top-level decls of this file. IP_NONE until
//                the query first runs.
typedef struct {
  ScopeId internal;
  ScopeId exported;
  IpIndex struct_type;
} NamespaceScopes;


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

  // Request-scoped tracking of slots set to QUERY_RUNNING. db_request_end
  // sweeps this list and resets any leftover RUNNING slots to EMPTY,
  // defending against bodies that exit without calling
  // db_query_succeed/fail (cancellation, future error patterns). Cleared
  // at db_request_begin and at the end of db_request_end's sweep. Pushed
  // by db_query_begin's COMPUTE return paths.
  Vec running_slots; // Vec<QueryRunningRef>

  // Typed wrapper dispatch — db_verify pulls a recorded dep via
  // recompute_dispatch[dep.kind](s, dep.key); the wrapper handles
  // cache-vs-recompute internally. Populated at db_init by
  // db_register_query_dispatch (src/db/query/dispatch.c — the only
  // file bridging engine and consumer types).
  RecomputeFn recompute_dispatch[QUERY_KIND_COUNT];

#ifdef ORE_DEBUG_QUERIES
  // Per-QueryKind counters: begin / cached_hit / compute / cycle /
  // error / recompute_due_to_untracked. Touched by db_query_begin,
  // db_query_succeed, db_query_fail, and db_verify. Dumped by
  // db_dump_query_stats. Only built under ORE_DEBUG_QUERIES — the
  // profile workload (tools/lsp_workload.c) reads these to compute
  // per-iteration deltas.
  QueryStats query_stats[QUERY_KIND_COUNT];
#endif

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
  //   node_data         — FileNodeData: a contiguous block of TinySpan[],
  //                        AstNodeId[], DefId[] all sized to
  //                        node_counts[f] (see the FileNodeData typedef).
  //   node_counts       — node count for this file's FileNodeData arrays.
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
  //   slots_node_to_def — per-file QUERY_NODE_TO_DECL slot. The node→DefId
  //                        reverse index (FileNodeData.defs) is that
  //                        query's output; see query/node_to_def.c.
  // 3-arg X-macro: (column name, element type, eviction action).
  //
  // The `evict` arg names an EVICT_* macro defined in
  // src/db/workspace/workspace.c. It runs on the per-row slot in
  // workspace_did_evict_source for each file backed by the evicted
  // source. EVICT_NOOP for columns that survive eviction (the ID
  // back-references, query slots — those are reset explicitly because
  // they need a partial-clear, not a full zero).
  //
  // Adding a new per-file column = one line here. Forgetting an
  // eviction policy = compile error (unknown EVICT_* macro). This
  // closes the silent-leak risk the open-coded eviction had: the
  // policy is co-located with the column declaration, single source
  // of truth.
  // ORDER MATTERS for eviction: any action that DEREFERENCES a pointer
  // into the per-file arena (notably EVICT_FREE_ASTSTORE, which reads
  // (*ASTStore)->kinds etc.) must run BEFORE EVICT_ARENA_FREE on the
  // `arenas` column. Actions that only NULL pointer slots
  // (EVICT_NULL_PTR_GENERIC) or zero FileArray fields without deref
  // (EVICT_ZERO_FILEARRAY) are safe in any position because they don't
  // touch the pointee. The declared order below — `asts` before
  // `arenas` — encodes this dependency. A reader who later inserts a
  // new column whose eviction derefs an arena-allocated pointee MUST
  // place it before `arenas`.
#define ORE_FILES_COLUMNS(X)                                              \
    X(ids,               FileId,             EVICT_NOOP)                  \
    X(source_id,         SourceId,           EVICT_NOOP)                  \
    X(module_id,         NamespaceId,        EVICT_NOOP)                  \
    X(asts,              struct ASTStore *,  EVICT_FREE_ASTSTORE)         \
    X(line_starts,       FileArray,          EVICT_ZERO_FILEARRAY)        \
    X(node_data,         FileNodeData,       EVICT_FILE_NODE_DATA_ZERO)   \
    X(node_counts,       uint32_t,           EVICT_ZERO_U32)              \
    X(trivia_tokens,     FileArray,          EVICT_ZERO_FILEARRAY)        \
    X(trivia_offsets,    FileArray,          EVICT_ZERO_FILEARRAY)        \
    X(ast_id_maps,       struct AstIdMap *,  EVICT_NULL_PTR_GENERIC)      \
    X(top_level_indices, FileArray,          EVICT_ZERO_FILEARRAY)        \
    X(imports,           FileArray,          EVICT_ZERO_FILEARRAY)        \
    X(arenas,            Arena,              EVICT_ARENA_FREE)            \
    X(slots_ast_hot,            struct QuerySlotHot,  EVICT_NOOP)         \
    X(slots_ast_cold,           struct QuerySlotCold, EVICT_NOOP)         \
    X(slots_node_to_def_hot,    struct QuerySlotHot,  EVICT_NOOP)         \
    X(slots_node_to_def_cold,   struct QuerySlotCold, EVICT_NOOP)         \
    X(slots_file_imports_hot,   struct QuerySlotHot,  EVICT_NOOP)         \
    X(slots_file_imports_cold,  struct QuerySlotCold, EVICT_NOOP)
  struct {
#define X(name, type, _evict) Vec name;
    ORE_FILES_COLUMNS(X)
#undef X
  } files;

  // MODULES — a thin aggregation over a set of files. Holds only
  // genuinely module-scoped state; its contents (top-level index /
  // exports) are DERIVED queries that aggregate over the module's
  // files (each recording a dep on that file's QUERY_FILE_AST, so an
  // edit to one file early-cuts the others).
  //
  // "Files belonging to module M" is NOT stored on the module row —
  // it's the back-ref `files.module_id` filtered down to M. The flat
  // file_pool / file_offsets pair this used to maintain is gone (its
  // construction-only growth limit was the Gap B blocker; per-module
  // Vecs would have been an SoA anti-pattern). db_get_namespace_files is
  // a filter scan over a dense u32 column.
  //
  //   ids           — pointer-stable module-query slot key.
  //   names         — Vec<StrId>.
  //   exports       — the QUERY_NAMESPACE_SCOPES result record per module
  //                   (NamespaceScopes): both the export scope (the query's
  //                   output) and the internal scope (its intermediate,
  //                   cached so a re-run reuses the same ScopeId).
  //                   SCOPE_ID_NONE until db_query_namespace_scopes first
  //                   runs. Internal scope collects every decl; export
  //                   scope mirrors only the public subset (importers
  //                   query export to stay stable across private edits).
  //   slots_index /  QUERY_TOP_LEVEL_INDEX / QUERY_NAMESPACE_SCOPES slots.
  //   slots_exports
#define ORE_NAMESPACES_COLUMNS(X)              \
    X(ids,             NamespaceId)            \
    X(names,           StrId)               \
    X(exports,         NamespaceScopes)       \
    X(slots_index_hot,    struct QuerySlotHot)  \
    X(slots_index_cold,   struct QuerySlotCold) \
    X(slots_exports_hot,  struct QuerySlotHot)  \
    X(slots_exports_cold, struct QuerySlotCold) \
    X(slots_namespace_type_hot,  struct QuerySlotHot)  \
    X(slots_namespace_type_cold, struct QuerySlotCold)
  struct {
#define X(name, type) Vec name;
    ORE_NAMESPACES_COLUMNS(X)
#undef X
  } namespaces;

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
    X(parent_modules, NamespaceId) \
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
    X(type,                  IpIndex)              \
    X(signature,             IpIndex)              \
    X(slot_type_hot,         struct QuerySlotHot)  \
    X(slot_type_cold,        struct QuerySlotCold) \
    X(slot_signature_hot,    struct QuerySlotHot)  \
    X(slot_signature_cold,   struct QuerySlotCold) \
    X(slot_infer_hot,        struct QuerySlotHot)  \
    X(slot_infer_cold,       struct QuerySlotCold) \
    X(slot_body_scopes_hot,  struct QuerySlotHot)  \
    X(slot_body_scopes_cold, struct QuerySlotCold) \
    X(body,                  FnBody)
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
    X(type,           IpIndex)              \
    X(slot_type_hot,  struct QuerySlotHot)  \
    X(slot_type_cold, struct QuerySlotCold)
  struct {
#define X(name, type) Vec name;
    ORE_STRUCTS_COLUMNS(X)
#undef X
  } structs;

#define ORE_UNIONS_COLUMNS(X) \
    X(type,           IpIndex)              \
    X(slot_type_hot,  struct QuerySlotHot)  \
    X(slot_type_cold, struct QuerySlotCold)
  struct {
#define X(name, type) Vec name;
    ORE_UNIONS_COLUMNS(X)
#undef X
  } unions;

#define ORE_ENUMS_COLUMNS(X) \
    X(type,           IpIndex)              \
    X(slot_type_hot,  struct QuerySlotHot)  \
    X(slot_type_cold, struct QuerySlotCold)
  struct {
#define X(name, type) Vec name;
    ORE_ENUMS_COLUMNS(X)
#undef X
  } enums;

#define ORE_EFFECTS_COLUMNS(X) \
    X(type,           IpIndex)              \
    X(slot_type_hot,  struct QuerySlotHot)  \
    X(slot_type_cold, struct QuerySlotCold)
  struct {
#define X(name, type) Vec name;
    ORE_EFFECTS_COLUMNS(X)
#undef X
  } effects;

#define ORE_HANDLERS_COLUMNS(X) \
    X(type,           IpIndex)              \
    X(slot_type_hot,  struct QuerySlotHot)  \
    X(slot_type_cold, struct QuerySlotCold)
  struct {
#define X(name, type) Vec name;
    ORE_HANDLERS_COLUMNS(X)
#undef X
  } handlers;

#define ORE_VARIABLES_COLUMNS(X) \
    X(type,           IpIndex)              \
    X(slot_type_hot,  struct QuerySlotHot)  \
    X(slot_type_cold, struct QuerySlotCold)
  struct {
#define X(name, type) Vec name;
    ORE_VARIABLES_COLUMNS(X)
#undef X
  } variables;

  // CONSTANT — db.constants. Adds slot_const_eval for the (declared,
  // not-yet-implemented) QUERY_CONST_EVAL.
#define ORE_CONSTANTS_COLUMNS(X) \
    X(type,                 IpIndex)              \
    X(slot_type_hot,        struct QuerySlotHot)  \
    X(slot_type_cold,       struct QuerySlotCold) \
    X(slot_const_eval_hot,  struct QuerySlotHot)  \
    X(slot_const_eval_cold, struct QuerySlotCold)
  struct {
#define X(name, type) Vec name;
    ORE_CONSTANTS_COLUMNS(X)
#undef X
  } constants;

  // HASHMAP-KEYED QUERIES — def_identity, resolve_ref, resolve_path.
  // Their slots live in dense Vec<QuerySlot> columns (like the per-kind
  // tables); db.def_by_identity / db.resolve_ref_cache /
  // db.resolve_path_cache route a packed u64 key to a row index here.
  // Row 0 of each is a reserved sentinel; `results` holds the per-row
  // cached DefId.
  struct {
    Vec results;    // Vec<DefId>
    Vec slots_hot;  // Vec<QuerySlotHot>
    Vec slots_cold; // Vec<QuerySlotCold>
  } def_identity;
  struct {
    Vec results;    // Vec<DefId>
    Vec slots_hot;  // Vec<QuerySlotHot>
    Vec slots_cold; // Vec<QuerySlotCold>
  } resolve_ref;
  struct {
    Vec results;    // Vec<DefId>
    Vec slots_hot;  // Vec<QuerySlotHot>
    Vec slots_cold; // Vec<QuerySlotCold>
  } resolve_path;
  // QUERY_DECL_AST — per-decl AST handle. Same routed-SoA shape; routed
  // by db.decl_ast_cache from a packed (file_local<<32 | ast_id) key.
  // results[row] holds the decl's current AstNodeId (the query's value).
  struct {
    Vec results;    // Vec<AstNodeId>
    Vec slots_hot;  // Vec<QuerySlotHot>
    Vec slots_cold; // Vec<QuerySlotCold>
  } decl_ast;

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
    X(owning_modules, NamespaceId)
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
    X(durability, Durability)               \
    X(evicted,    uint8_t)                  \
    X(is_virtual, uint8_t)
  struct {
#define X(name, type) Vec name;
    ORE_SOURCES_COLUMNS(X)
#undef X
  } sources;

  /* --- COLDER: Sparse Caches (HashMaps) --------------------------------- */
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
  HashMap resolve_path_cache;  // interned dotted-path StrId → db.resolve_path row
  HashMap decl_ast_cache;      // (file_local<<32 | ast_id) → db.decl_ast row
  HashMap def_by_identity;     // (nsid.idx<<32 | ast_id.idx) → db.def_identity row
  HashMap resolve_ref_cache;   // (scope.idx<<32 | name.idx) → db.resolve_ref row
  HashMap comptime_call_cache;

  // Centralized diagnostics. diag_lists is a dense Vec<DiagList> (row 0 a
  // reserved sentinel); `diags` routes a (QueryKind, key) analysis-unit
  // u64 to a diag_lists row. Per-file / all collection walks the dense
  // Vec — the map only routes emission. Keyed by the packed u64 from
  // diag_unit_key; see the DiagList typedef above and src/db/diag/diag.h.
  Vec     diag_lists;  // Vec<DiagList> — row 0 reserved sentinel
  HashMap diags;       // unit-key u64 → diag_lists row (>= 1)
};

void db_init(struct db *s);
void db_free(struct db *s);

// Wires every active QueryKind's recompute thunk into s->recompute_dispatch.
// Defined in src/db/query/dispatch.c — the single file that knows about
// both the engine's QueryKind enum and every wrapper's typed signature.
// Called from db_init.
void db_register_query_dispatch(struct db *s);

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
// Virtual-source admission: same row allocation but NOT inserted into
// source_by_path. Synthetic names (e.g. "comptime://gen_1.ore") are
// addressable only via the returned SourceId — they do not resolve
// through @import("path"). is_virtual flag set on the row.
SourceId db_admit_virtual_source(struct db *s, const char *synthetic_name,
                                  size_t name_len, const char *text,
                                  size_t text_len);
bool     db_set_source_text(struct db *s, SourceId src,
                            const char *text, size_t text_len);
void     db_set_source_durability(struct db *s, SourceId src, uint8_t dur);

// --- Setters: file -----------------------------------------------------------
FileId   db_create_file(struct db *s, SourceId src, NamespaceId owner);
// Virtual-file allocation: FileId has the virtual bit set; no
// TOP_LEVEL_INDEX gate-bump (owner is expected to be a fresh
// namespace). Pair with db_admit_virtual_source.
FileId   db_create_virtual_file(struct db *s, SourceId src, NamespaceId owner);

// --- Setters: module ---------------------------------------------------------
// Allocate a new module row. File-as-namespace model: every file gets
// its own fresh NamespaceId; sibling files do NOT share a module.
NamespaceId db_create_namespace(struct db *s);

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
bool        db_get_source_evicted  (struct db *s, SourceId src);
bool        db_get_source_is_virtual(struct db *s, SourceId src);
SourceId    db_lookup_source_by_path(struct db *s, const char *path,
                                     size_t path_len);

// --- Getters: file -----------------------------------------------------------
SourceId db_get_file_source      (struct db *s, FileId fid);
NamespaceId db_get_file_namespace      (struct db *s, FileId fid);
FileId   db_lookup_file_by_source(struct db *s, SourceId src);
struct ASTStore   *db_get_file_ast        (struct db *s, FileId fid);
struct AstIdMap   *db_get_file_ast_id_map (struct db *s, FileId fid);
TinySpan db_get_node_span(struct db *s, FileId fid, AstNodeId node);
// Enclosing top-level DefId for an AST node. Walks the parent chain
// up to the nearest decl-root (stamped by db_query_namespace_scopes
// into FileNodeData.defs). DEF_ID_NONE if the node isn't inside
// any top-level decl, or module_exports hasn't run for this file.
DefId    db_get_def_for_node(struct db *s, FileId fid, AstNodeId node);

// --- Getters: module ---------------------------------------------------------
const FileId *db_get_namespace_files(struct db *s, NamespaceId nsid,
                                  uint32_t *out_count);
// Export / internal scope of a module (the QUERY_NAMESPACE_SCOPES result).
// SCOPE_ID_NONE until that query has run for the module.
ScopeId  db_get_namespace_internal_scope(struct db *s, NamespaceId nsid);

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
