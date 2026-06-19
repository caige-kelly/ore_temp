#ifndef ORE_DB_H
#define ORE_DB_H

#include <assert.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "./diag/ast_id.h"          // DeclAstIdMap (Phase P / Phase-3.1 follow-up)
#include "./diag/diag.h"            // DiagBundle (Phase P fn_body_diags)
#include "./ids/ids.h"
#include "./intern_pool/intern_pool.h"
#include "./query/engine.h"
#include "../support/data_structure/arena.h"
#include "../support/data_structure/hashmap.h"
#include "../support/data_structure/paged_vec.h"
#include "../support/data_structure/stringpool.h"
#include "../support/data_structure/vec.h"
#include "../syntax/syntax.h"
#include "names.inc"

// Per-file artifacts owned elsewhere (parser / workspace) — db.files
// stores typed pointers to them, so forward-declare the tags here.
struct GreenNode;
struct NodeCache;

/* --- Column Types --- */

// Three-state visibility, packed into the 2-bit META_VIS_MASK on DefMeta:
//   VIS_PRIVATE  — name + internals private (default; zero value).
//   VIS_PUBLIC   — name + internals exported.
//   VIS_ABSTRACT — name exported, internals (fields / effect ops) private.
//                  The "opaque type" / "opaque effect" case: callers can
//                  refer to the type by name but cannot construct or pattern-
//                  match on its components.
// VIS_ABSTRACT is bit pattern 0b10 in the 2-bit mask; the fourth bit pattern
// 0b11 is reserved.
typedef enum : uint8_t {
  VIS_PRIVATE  = 0,
  VIS_PUBLIC   = 1,
  VIS_ABSTRACT = 2,
} Visibility;

// Input durability — how often an input of this kind changes (Salsa
// tiers). Numeric value = volatility rank: LOW is the most volatile
// (workspace file text, edited constantly), HIGH the most stable
// (library sources). A derived query's durability is the MIN (most
// volatile) over its inputs. The Durability enum + DUR_COUNT live in
// engine.h since it's an engine-level concept; setters here take them
// as parameters.

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
  DefId def;  // The decl's canonical DefId. SAFE to store here (unlike the
              // old node_ptr) because def_identity is AstId-keyed: a decl's
              // DefId is reparse-STABLE (same kind+name → same DefId), so it
              // never goes stale within the scope. namespace_scopes writes
              // it (calling def_identity per decl); resolve_ref reads it.
              // The current syntax location is NOT stored here — it's
              // position-dependent and obtained via top_level_entry when
              // needed (keeps this scope binding behind a position-
              // independent fingerprint).
} DeclEntry;

// A nominal aggregate (struct OR union) member: name → resolved type. Stored
// in the shared db.aggregate_field_pool (a recompute-friendly pool, per-def
// (field_lo, field_len) range on the structs/unions table — the decl_pool
// pattern), NOT in the intern pool: an aggregate's IpIndex is a stable inline
// identity (zir = def.idx), and its member list churns on recompute, so it
// lives in a db-side pool that the pool compactor reclaims.
typedef struct {
  StrId   name;
  IpIndex type;
} AggregateFieldEntry;

// Effect-operation sort, named for the surface keywords (`val`/`direct`/
// `final-ctl`/`ctl`; `direct` is Koka's tail-resumptive `fun`). The numeric
// ORDER is load-bearing: it drives the handler sort-compatibility rule
// (a clause may use ≤ the declared op's control; using more is an error) —
// so a clause-sort ≤ decl-sort is OK, > is rejected. `val` is strict (a `val`
// op needs a `val` clause). Stored per effect op in db.effect_op_sorts; also
// the perform-shape selector for future evidence-vector codegen
// (ctl→yield, direct→inline call, final-ctl→throw, val→constant).
typedef enum {
  OP_VAL = 0,
  OP_DIRECT = 1,
  OP_FINAL_CTL = 2,
  OP_CTL = 3,
} OpSort;

// A nominal enum variant: name → value. Stored in db.enum_variant_pool,
// per-enum (variant_lo, variant_len) range. Same rationale as StructFieldEntry.
typedef struct {
  StrId   name;
  int64_t value;
} EnumVariantEntry;

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

// Diagnostic anchor: DiagAnchor (12 bytes — file:16 + syntax_kind:16 +
// start:32 + length:32). See src/db/diag/diag.h. Reparse-stable: render-
// time resolution rebinds via syntax_node_ptr_resolve when the kind+
// length pair still matches a node in the current tree, otherwise falls
// back to the captured byte range. This is the inverse of the Phase-4
// "byte-range is sufficient" decision — fine-grained query memoization
// caches per-fn diags across edits, so without node-pointer anchoring a
// stale diag for an untouched fn drifts when a neighbor's whitespace
// edit shifts byte offsets. Bulk rendering threads a DiagResolver to
// amortize the per-file red-root build across all diags in a publish;
// db_resolve_span remains as the low-level TinySpan → (line, col)
// primitive the resolver delegates to.

typedef struct {
#define X(id, name) StrId id;
    PRIMITIVE_LIST(X)
    BUILTIN_LIST(X)
    FIELD_LIST(X)
    CONTEXT_LIST(X)
#undef X
} DbNames;

// def_identity and resolve_ref do not use embedded-slot entry structs:
// their slots live in dense db.def_identity / db.resolve_ref SoA
// columns, routed by the db.def_by_identity / db.resolve_ref_cache
// HashMaps.

// --- Diagnostics --------------------------------------------------------
//
// Phase P (post-cutover): diagnostics live in per-query-result DiagBundle
// columns (db.files.parse_diags, db.fns.fn_body_diags,
// db.fns.signature_diags, db.defs.type_of_decl_diags,
// db.namespaces.check_diags). The legacy centralized DiagList side store
// (db.diag_lists, db.diags HashMap, diag_lists_by_file index) was deleted;
// db_collect_diags_for_file walks the column-owned bundles directly.
//
// Emit is single-path via db_emit (in src/db/diag/sink.c). Each emit
// owning query installs a DiagSink on its QueryFrame at compute entry;
// db_emit asserts the active frame has one. Driver-level emits (CHECK)
// use diag_sink_emit_tagged with an explicit sink.

// --- Body scope tree (rust-analyzer ExprScopes, SyntaxNodePtr-keyed) ---
//
// Per-fn lexical scope structure. Built by db_query_body_scopes (sema-
// side impl in sema/body_scopes.c). The tree is stored flat in two
// shared db pools; the per-fn (off,len) ranges AND the SyntaxNodePtr→
// scope_id HashMap both live inside FnBody (the BODY_SCOPES query's
// result struct) — H11 folded the scope_map into FnBody so the slot
// owns the entire result.
//
//   db.body_scope_rows  — ScopeRow. Each row has a parent (the tree).
//                         scope 0 = fn-root (holds params).
//   db.body_scope_binds — ScopedBind. Each bind tags its owning scope_id
//                         so multiple scopes can interleave bind pushes.
//   FnBody.scope_map    — HashMap<syntax_node_ptr_hash, ScopeId>.
//                         Sparse: only nodes that participate in a scope
//                         get tagged. Embedded directly in the BODY_SCOPES
//                         result struct so orphan reclamation can free it
//                         atomically with the rest of the result.
//
// Lookup: scope = body.scope_map[ptr]; scan binds for matches in that
// scope (latest wins for shadows); on miss, walk to parent scope; repeat.
// Cost: one hash + O(depth × binds_count). Bodies are small.
#define BODY_SCOPE_NONE ((uint32_t)0xFFFFFFFF)

typedef struct {
  uint32_t       parent;          // BODY_SCOPE_NONE for the root
  uint32_t       block_node_rel;  // RelAstId of the block node (debug + LSP)
} ScopeRow;

typedef struct {
  uint32_t      scope_id;         // which ScopeRow this bind belongs to
  StrId         name;
  uint32_t      bind_site_rel;    // RelAstId of the node that introduces the name
} ScopedBind;

// BODY_SCOPES result struct. Holds:
//   - (off, len) handles into the shared body_scope_* pools (the
//     dense per-fn scope tree + binds).
//   - the per-fn SyntaxNodePtr → ScopeId HashMap (was db.fns.scope_map;
//     folded in by H11 so the slot owns the whole result).
// Orphan reclamation of a BODY_SCOPES slot must hashmap_free(scope_map)
// before zeroing the row — see engine_compact.c's per-kind free hook.
typedef struct {
  uint32_t scope_off, scope_len;   // -> db.body_scope_rows
  uint32_t bind_off,  bind_len;    // -> db.body_scope_binds
  HashMap  scope_map;              // SyntaxNodePtr → ScopeId
} FnBody;

// Per-decl resolved-types table — the rust-analyzer InferenceResult
// pattern, keyed by SyntaxNodePtr. Each query that types a sub-tree
// (infer_body, fn_signature, struct_field_types) builds one of these
// tables and stores it on the matching per-kind column
// (db.fns.body_node_types, db.fns.signature_node_types,
// db.structs.field_node_types).
//
// Phase 4 redesign: the dense `pool[off + (node.idx - node_min)]`
// indexing from the flat-AST era is replaced by a per-decl
// HashMap<SyntaxNodePtr, IpIndex>. SyntaxNode pointers aren't
// sequential integers, so dense indexing isn't possible; HashMap is the
// natural fit. Lookup cost: one hash + bucket probe. Storage:
// proportional to the body's node count (same as before).
//
// Empty / cycle sentinel: `types.bucket_count == 0` → all lookups
// return IP_NONE. db.empty_node_types_range is the canonical zero
// value (see db_init).
typedef struct {
  HashMap types;
} NodeTypesRange;

// ---- H11: per-query result structs (slot-owned) ---------------------
//
// Each result struct is what its owning query's slot memoizes. Side-data
// (node-type tables) that used to live in separate columns is folded
// here so a single row in the result column carries the whole result.
// The owning query's wrapper returns a `const T *` borrowed pointer into
// the result column. Engine-owned memory.
//
// Orphan-reclaim semantics: structs containing HashMap or NodeTypesRange
// fields are zeroed by reclaim_slot AFTER the embedded HashMap is freed.
// engine_compact.c's per-kind free hook handles this — see reclaim_slot.

typedef struct {
  IpIndex        type;        // the function type (IpFnType)
  NodeTypesRange node_types;  // per-sig-position resolved types
  bool           effect_annotated; // SHAPE: was an `<E>` row written? If not,
                                    // FN_SIGNATURE infers the effect from the
                                    // body (Stage 2b). Not folded into the fp
                                    // (fp tracks type.v).
} FnSignature;

// Monomorphization — QUERY_INFER_INSTANCE's result: a generic fn body
// re-checked with each anytype hole bound to its concrete arg. `ret_type`
// is the concrete return (apply_type_subst of the generic return);
// `node_types` is the per-instance body node→type map (duck-typed against
// concretes). Diags live in a SEPARATE db.instances.diags column (mirror
// of INFER_BODY's body_node_types + fn_body_diags split), so a result
// overwrite never clobbers the bundle.
typedef struct {
  IpIndex        ret_type;
  NodeTypesRange node_types;
} InstanceResult;

typedef struct {
  IpIndex        type;
  NodeTypesRange value_node_types;
} VariableType;

typedef struct {
  IpIndex        type;
  NodeTypesRange value_node_types;
} ConstantType;

typedef struct {
  IpIndex        type;
  NodeTypesRange field_node_types;
} StructType;

// A per-file flat array: a {pointer, count} header (one per file row in a
// Vec column) over a separately-allocated body. `data`'s element type is
// column-specific — uint32_t for line_starts, FileImport for imports.
//
// Body ownership differs by column:
//   - line_starts: body lives in the per-file arena (files.arenas[file]),
//     which QUERY_LINE_INDEX resets on each reparse (O(1) isolation, zero
//     dead slack). EVICT_ZERO_FILEARRAY — the arena owns the bytes.
//   - imports: body is a standalone malloc, replaced wholesale on
//     QUERY_FILE_IMPORTS recompute (free old, malloc new) and freed on
//     evict (EVICT_FREE_FILEARRAY). Malloc rather than the per-file arena
//     because line_index OWNS that arena (its reset would clobber an
//     imports body sharing it); malloc is also denser here — a file with
//     no imports stores {NULL, 0}, zero body bytes.
typedef struct {
  void    *data;
  uint32_t count;
} FileArray;

// One @import / import-decl site discovered by QUERY_FILE_IMPORTS.
// `path` is the interned import-path string (feeds workspace_resolve_import);
// `site` anchors import diagnostics back to the source.
typedef struct {
  StrId         path;
  SyntaxNodePtr site;
} FileImport;

/* --- Definition Metadata (8 bits) --- */
//
// Modifier bits are captured by the parser regardless of whether sema
// currently enforces them — IR is faithful to syntax, sema decides per-
// phase what to act on. Adding enforcement later is a sema-only edit; if
// the bit weren't captured here, every parser/IR site would need rework.
//
// Visibility is a 3-state enum (private/public/abstract) packed into the
// 2-bit VIS_MASK — abstract is NOT a separate bit. See the Visibility
// enum above for the encoding.
typedef uint8_t DefMeta;
#define META_VIS_MASK 0x03 // Bits 0-1 (Visibility — 3 states, see enum)
#define META_COMPTIME 0x04 // Bit 2  (is_comptime)
#define META_SCOPED   0x08 // Bit 3  (scoped — value-level escape marker)
#define META_NAMED    0x10 // Bit 4  (named effect / first-class handler value)
#define META_LINEAR   0x20 // Bit 5  (linear effect — all ops are val/fun)
                           // Bit 6  reserved (free; was META_ABSTRACT before
                           //                 abstract folded into VIS_MASK)
                           // Bit 7  reserved (free; was META_DISTINCT before
                           //                 distinct became its own KIND_DISTINCT)

// Reparse-stable identity for a top-level decl wrapper in a file.
// `id` is the content-addressed AstId (kind, name) — the stable identity
// downstream (DefId derivation, def_identity) keys on. `node_ptr` (kind,
// byte range) is the CURRENT location, refreshed each reparse, resolved
// against `files.green_roots[fid]` via syntax_node_ptr_resolve.
typedef struct {
  AstId         id;
  StrId         name;
  FileId        file;      // the defining file — lets type-layer queries
                           // resolve node_ptr against file_ast(file) without
                           // re-touching NAMESPACE_ITEMS. {0} when NOT_FOUND.
  SyntaxNodePtr node_ptr;
  DefMeta       meta;
} TopLevelEntry;

// One enumerated top-level item of a namespace, produced by
// QUERY_NAMESPACE_ITEMS (the per-namespace items index — the single
// memoized, dep-tracked "what are the top-level items of namespace N?").
// Items are stored SORTED BY `id` (AstId), so def_identity binary-searches
// and the index fingerprint (a fold of the AstIds) is a reorder-stable
// MEMBERSHIP signal: it changes on add/remove/rename, NOT on a content edit
// — so the name layer (def_identity, namespace_scopes) that depends on it is
// firewalled from body edits. `name` is the resolution lookup key; `file`
// the defining file; `ptr` the CURRENT location (refreshed each recompute).
// No content hash here: the per-decl content firewall (the trivia-excluding
// structural hash) is computed by top_level_entry, which holds the file_ast.
typedef struct {
  AstId         id;
  StrId         name;
  FileId        file;
  SyntaxNodePtr ptr;
  DefMeta       meta;
  DefKind       kind;  // the decl's SEMANTIC classification (KIND_FUNCTION/
                       // KIND_STRUCT/…), computed by the walk from the RHS
                       // value for `::`/`:=` binds — NOT the resolved node's
                       // literal SyntaxKind (item.kind == KIND_FUNCTION while
                       // ptr resolves to SK_BIND_DECL). def_identity reads it
                       // directly to classify the DefId; consumers dispatch on
                       // db_def_kind, never on syntax_node_kind(resolve(ptr)).
} NamespaceItem;

// Per-namespace (per-file) scope record, one per NamespaceId in
// db.namespaces.exports (column name is historical — it now holds only the
// internal scope). Owned exclusively by QUERY_NAMESPACE_SCOPES.
//
//   internal   — every top-level decl in scope; used to resolve bare
//                identifiers WITHIN this file. SCOPE_ID_NONE until
//                db_query_namespace_scopes first runs.
//
// The public export scope is NOT here — a namespace's exports are the
// NAMESPACE_TYPE query's nominal member list (db.namespaces.namespace_type[]
// + namespace_field_pool); the old always-NONE `exported` field was removed.
// H23: the former `struct_type` field (NAMESPACE_TYPE's result) likewise lives
// in its own column. Each query owns its own named result column.
typedef struct {
  ScopeId internal;
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
  // durability]. This is the SKIP-OPTIMIZATION layer over per-input
  // deps: db_engine_verify early-cuts a slot when no input at its
  // durability tier changed since it was last verified — without
  // walking its dependency graph at all. Correctness still comes from
  // the per-input dependency fingerprints (see QUERY_SOURCE_TEXT); the
  // tier only lets verify skip the walk in the common case. Initialized
  // to the db's starting revision (1) so the first revalidation is exact.
  _Atomic uint64_t dur_last_changed[DUR_COUNT];

  uint32_t comptime_depth_limit;

// Monomorphization recursion bound — the max nesting of db_query_infer_instance
// before the SK_CALL_EXPR demand bails with "monomorphization too deep". Catches
// infinite instantiation (`grow(x) -> grow(&x)` → unbounded DISTINCT instances),
// which the engine's cycle detection CANNOT (each instance key is a distinct
// slot, not a re-entered one). Kept well under query_stack capacity (each level
// keeps several query frames live — see ids.c). Distinct from
// comptime_depth_limit (the general comptime-eval bound); 64 is generous for any
// real generic recursion.
#define ORE_MONO_DEPTH_LIMIT 64u

  // Monomorphization — LIVE recursion depth of in-flight db_query_infer_instance
  // compute frames (a call-stack depth on shared db state, since SemaCtx does
  // not cross the query boundary). Incremented after the instance GUARD,
  // decremented before return; memoized hits never re-run the body so it only
  // grows along the live recursion path. Bailed against ORE_MONO_DEPTH_LIMIT.
  uint32_t mono_depth;

  /* --- Cancellation --- */
  _Alignas(64) atomic_bool cancel_requested;

  /* --- WARM: Global Managers -------------------------------------------- */
  _Alignas(64) Arena arena;
  Arena request_arena;
  StringPool strings;
  InternPool intern;
  // Hash-cons interner for green-tree nodes/tokens. One per workspace;
  // outlives every parse so structural sharing accumulates across files
  // and reparses. Allocated in db_init via node_cache_new, freed in
  // db_free via node_cache_destroy.
  struct NodeCache *node_cache;
  Vec query_stack; // Vec<QueryFrame>

  // Request-scoped tracking of slots set to QUERY_RUNNING. db_request_end
  // sweeps this list and resets any leftover RUNNING slots to EMPTY,
  // defending against bodies that exit without calling
  // db_query_succeed/fail (cancellation, future error patterns). Cleared
  // at db_request_begin and at the end of db_request_end's sweep. Pushed
  // by db_query_begin's COMPUTE return paths.
  Vec running_slots; // Vec<QueryRunningRef>

  // Typed wrapper dispatch table now lives in engine_dispatch.c as
  // `extern const RecomputeFn db_engine_recompute_dispatch[]`. No need
  // for a per-db copy: the table is static, immutable, generated by
  // the ORE_QUERY_KINDS X-macro.

  // Per-QueryKind counters. ALWAYS available (not gated on debug builds)
  // — production telemetry. LSP profilers, debug extensions, and tests
  // all read these. Touched by db_query_begin/succeed/fail and by
  // db_engine_compact (orphan_reclaimed). Cost: 6 u64s × QUERY_KIND_COUNT
  // ≈ 624 bytes.
  QueryStats query_stats[QUERY_KIND_COUNT];

  // Pre-interned StrIds for hot builtin + contextual-keyword identifiers.
  // Populated at db_init from BUILTIN_LIST / CONTEXT_LIST (names.inc).
  DbNames names;

  // Primitive type identifiers (u8, bool, usize, ...) as real DefIds.
  // Allocated at db_init in a synthetic scope whose ScopeId is recorded
  // here. Every namespace's internal scope is parented to this scope at
  // module_exports time, so resolve_ref's existing parent walk finds
  // primitives by name without any special-case lookup table. The
  // primitive DefIds are allocated contiguously starting at
  // first_primitive_def — so resolve_ref can detect "this DeclEntry is
  // synthetic" by scope identity, and type_of_def can map a primitive
  // DefId back to its IpIndex by (def.idx - first_primitive_def.idx).
  ScopeId primitives_scope;
  DefId   first_primitive_def;
  uint32_t primitive_count;

  // Follow-up #3 — compile-time `builtin` namespace.
  // Populated at db_init via workspace_admit_virtual, which registers
  // a synthetic source file named "builtin" containing the Os/Arch/Mode
  // enum decls + the host-detected const bindings (`os :: pub Os.macos`
  // etc.). The virtual file gets a real NamespaceId via the existing
  // 1:1 file-to-namespace pairing in workspace.c, so parse / namespace_type
  // / def_identity / type_of_decl all handle it like a user file — no
  // synthetic-DefId short-circuits, no compactor whitelists.
  //
  // User code reaches the namespace via `builtin :: @import("builtin")`,
  // routed through the existing BUILTIN_IMPORT handler. A small fast-path
  // in workspace_resolve_import (virtual_by_name shortcut) short-circuits
  // disk resolution for pre-admitted virtual names. Subsequent `builtin.os`
  // field access hits SK_FIELD_EXPR's IP_TAG_NAMESPACE_TYPE arm; const_eval's
  // generic CONST_NAMESPACE walk folds the field to its const value.
  NamespaceId builtin_namespace;

  // Auto-imported prelude (Koka's std/core analogue): a synthetic virtual
  // file holding the primitive effect LABELS (`asm`, and later the `io`
  // family + aliases). Its `pub` members are folded into every namespace's
  // internal scope by db_query_namespace_scopes, so `<asm>` resolves with no
  // `@import`.
  NamespaceId prelude_namespace;
  // The 1-label `<asm>` effect row, lazily resolved from the prelude's `asm`
  // effect and cached (pool-stable). Unioned into a body's effect row at each
  // inline-asm node so an asm-bodied fn honestly performs `<asm>`. IP_NONE
  // until first built. See db_asm_effect_row (type.c).
  IpIndex asm_effect_row;

  // Pool-compaction triggers. Each shared pool grows monotonically as
  // queries re-run; old ranges become unreachable but stay in the pool.
  // At db_request_end we check `pool.count > last_compacted * 2 &&
  // pool.count > MIN_THRESHOLD` and compact if so. After compaction,
  // last_compacted is updated to the new (smaller) count. Trigger is
  // per-pool so an unchanged pool doesn't pay the mark-and-copy cost.
  uint32_t last_compacted_body_scope_rows_count;
  uint32_t last_compacted_body_scope_binds_count;
  uint32_t last_compacted_decl_pool_count;
  uint32_t last_compacted_aggregate_field_pool_count;  // D2.2
  uint32_t last_compacted_effect_op_pool_count;        // effect-op SoA
  uint32_t last_compacted_enum_variant_pool_count;     // D2.2
  uint32_t last_compacted_namespace_field_pool_count;  // D2.2

  // Per-pool-family compaction stats. Indexed 0=body_scope (rows + binds
  // share one counter — they compact together), 1=decl_pool,
  // 2=aggregate_field, 3=enum_variant, 4=namespace_field. Surfaced via the
  // profile-workload harness to validate (a) compactions actually fire,
  // (b) bytes reclaimed matches expected growth pattern, (c) total time
  // spent in compaction stays amortized.
  struct {
    uint64_t n_compactions[5];
    uint64_t bytes_reclaimed[5];
    uint64_t total_ns[5];
  } compact_stats;

  // Runtime-overridable compaction trigger threshold. Defaults to
  // ORE_COMPACT_MIN_THRESHOLD; the profile-workload harness can lower
  // it (e.g., to 0 or 100) for stress testing without rebuilding.
  uint32_t compact_min_threshold;

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
  //                        QUERY_FILE_AST slot key (engine's locate derefs).
  //   source_id         — backing source (text/version/hash).
  //   module_id         — owning module (back-ref).
  //   green_roots       — struct GreenNode *: the lossless concrete syntax
  //                        tree rooted at SK_SOURCE_FILE, emitted by
  //                        parse_file_green. Owns +1 refcount per file;
  //                        released on eviction via green_node_release.
  //   line_starts       — FileArray of uint32_t[] line-start byte offsets
  //                        in files.arenas[f]; built by the lexer, consumed
  //                        by diagnostic / LSP-position derivation.
  //   imports           — FileArray of FileImport[]: the file's @import
  //                        sites (path StrId + anchor), built by
  //                        QUERY_FILE_IMPORTS. Body is a STANDALONE malloc
  //                        (not arena-backed — see FileArray), replaced
  //                        wholesale on recompute, freed on evict.
  //   arenas            — per-file durable arena: hosts arena-backed
  //                        FileArray bodies (line_starts). arena_reset
  //                        at the top of QUERY_LINE_INDEX before a refill
  //                        (O(1) per-file isolation), arena_free in
  //                        db_ids_free.
  //   tokens            — FileArray of Token[] in files.arenas[f]: the
  //                        unified document-order stream from layout_stream
  //                        (trivia + virtual layout + real tokens, every
  //                        token carrying its full SK_* kind).
  //   slots_ast         — per-file QUERY_FILE_AST slot. Dense PagedVec
  //                        column so the revalidation walker iterates one
  //                        kind densely. PagedVec keeps slot pointers
  //                        stable across pushes; the engine re-resolves
  //                        via (kind, FileId) on every db_query_begin
  //                        regardless (no caller-side caching).
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
  // into the per-file arena must run BEFORE EVICT_ARENA_FREE on the
  // `arenas` column. Actions that only NULL pointer slots
  // (EVICT_NULL_PTR_GENERIC), zero FileArray fields without deref
  // (EVICT_ZERO_FILEARRAY), or release standalone heap allocations
  // (EVICT_RELEASE_GREEN) are safe in any position because they don't
  // touch the per-file arena. A reader who later inserts a new column
  // whose eviction derefs an arena-allocated pointee MUST place it
  // before `arenas`.
// Vec-backed input/output columns — file metadata, lossless tree, per-
// file arena-hosted arrays. Indexed by FileId; readers don't hold
// pointers across mutations.
#define ORE_FILES_COLUMNS(X)                                              \
    X(ids,               FileId,             EVICT_NOOP)                  \
    X(source_id,         SourceId,           EVICT_NOOP)                  \
    X(module_id,         NamespaceId,        EVICT_NOOP)                  \
    X(green_roots,       struct GreenNode *, EVICT_RELEASE_GREEN)         \
    X(line_starts,       FileArray,          EVICT_ZERO_FILEARRAY)        \
    X(imports,           FileArray,          EVICT_FREE_FILEARRAY)

// PagedVec-backed evictable columns. Promoted from ORE_FILES_COLUMNS to
// PagedVec because consumers (FILE_AST's sink_open) capture interior
// pointers (&bundle->items, &bundle->args_arena) that MUST survive
// subsequent db_create_file / db_create_def / vec_push_zero realloc
// across other Vec columns. PagedVec pages don't relocate under push,
// fixing a latent UAF in the diag-emit path.
//
// X-macro shape mirrors ORE_FILES_COLUMNS for the eviction handler so
// workspace.c's EVICT_FREE_DIAG_BUNDLE_PAGED can drive per-row cleanup.
//
// `arenas` promoted as part of the same UAF audit (file.c:117-132 and
// :190-202): db_create_file_impl and db_create_virtual_file capture
// `&arena` from vec_get then call arena_init; a second file creation
// can realloc the Vec and invalidate the pointer. B1-revised's two
// sequential workspace_admit_virtual calls (one for @target, one for
// @build) would hit this directly without the promotion.
#define ORE_FILES_PAGED_DIAG_COLUMNS(X)                                   \
    X(parse_diags,       DiagBundle,         EVICT_FREE_DIAG_BUNDLE_PAGED) \
    X(arenas,            Arena,              EVICT_ARENA_FREE_PAGED)

// PagedVec-backed slot columns — pointer-stable across pushes (the
// engine may push to other slot columns during a sub-query call, and
// readers must hold their slot pointers across that). Split out of
// ORE_FILES_COLUMNS by the A2 PagedVec migration. No eviction action
// here: slot eviction is handled by the engine's reclaim path.
#define ORE_FILES_SLOT_COLUMNS(X)                                         \
    X(slots_ast_hot,            struct QuerySlotHot)                      \
    X(slots_ast_cold,           struct QuerySlotCold)                     \
    X(slots_line_index_hot,     struct QuerySlotHot)                      \
    X(slots_line_index_cold,    struct QuerySlotCold)                     \
    X(slots_file_imports_hot,   struct QuerySlotHot)                      \
    X(slots_file_imports_cold,  struct QuerySlotCold)
  struct {
#define X(name, type, _evict) Vec name;
    ORE_FILES_COLUMNS(X)
#undef X
#define X(name, type, _evict) PagedVec name;
    ORE_FILES_PAGED_DIAG_COLUMNS(X)
#undef X
#define X(name, type) PagedVec name;
    ORE_FILES_SLOT_COLUMNS(X)
#undef X
  } files;

  // MODULES — a thin aggregation over a set of files. Holds only
  // genuinely module-scoped state; its contents (top-level index /
  // exports) are DERIVED queries that aggregate over the module's
  // files (each recording a dep on that file's QUERY_FILE_AST, so an
  // edit to one file early-cuts the others).
  //
  // "Files belonging to module M" — the authoritative record is the
  // back-ref `files.module_id` filtered to M, mirrored by the per-module
  // `member_files` Vec<FileId> reverse index (maintained in file_set_add /
  // db_namespace_remove_file). db_get_namespace_files reads member_files
  // (O(files-in-namespace)), not a scan over every file (the S1 fix). The
  // old flat file_pool / file_offsets pair is gone — its append-to-last-
  // module-only growth limit was the Gap B blocker.
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
  //   slots_*        QUERY_NAMESPACE_SCOPES / QUERY_NAMESPACE_TYPE
  //                  slots. Per-name TOP_LEVEL_ENTRY slots live in
  //                  db.top_level_entry (HashMap-routed).
// Vec-backed namespace metadata + per-namespace results. `items` is a
// FileArray whose body is a STANDALONE malloc of NamespaceItem[] (the
// QUERY_NAMESPACE_ITEMS result) — like files.imports: replaced wholesale
// on recompute, freed at teardown in db_ids_free. No evict handler:
// namespaces have no per-namespace eviction path.
#define ORE_NAMESPACES_COLUMNS(X)                               \
    X(ids,                       NamespaceId)                   \
    X(names,                     StrId)                         \
    X(exports,                   NamespaceScopes)               \
    X(namespace_type,            IpIndex)                       \
    X(field_lo,                  uint32_t)                      \
    X(field_len,                 uint32_t)                      \
    X(items,                     FileArray)                     \
    X(member_files,              Vec)

// PagedVec-backed for the same UAF reason as ORE_FILES_PAGED_DIAG_COLUMNS.
#define ORE_NAMESPACES_PAGED_DIAG_COLUMNS(X)                              \
    X(check_diags,               DiagBundle)

// PagedVec-backed slot columns. Pointer-stable across pushes (see
// ORE_FILES_SLOT_COLUMNS docstring).
#define ORE_NAMESPACES_SLOT_COLUMNS(X)                          \
    X(slots_file_set_hot,        struct QuerySlotHot)           \
    X(slots_file_set_cold,       struct QuerySlotCold)          \
    X(slots_exports_hot,         struct QuerySlotHot)           \
    X(slots_exports_cold,        struct QuerySlotCold)          \
    X(slots_namespace_type_hot,  struct QuerySlotHot)           \
    X(slots_namespace_type_cold, struct QuerySlotCold)          \
    X(slots_namespace_items_hot, struct QuerySlotHot)           \
    X(slots_namespace_items_cold,struct QuerySlotCold)          \
    /* QUERY_CHECK — a driver-managed diagnostic-owner slot (INPUT-class,    */ \
    /* like FILE_SET): the check driver stamps it live via db_input_set and  */ \
    /* owns the unused-decl warnings emitted to its unit. Never computed /   */ \
    /* db_query_begin'd, so the engine never auto-clears its diags.          */ \
    X(slots_check_hot,           struct QuerySlotHot)           \
    X(slots_check_cold,          struct QuerySlotCold)
  struct {
#define X(name, type) Vec name;
    ORE_NAMESPACES_COLUMNS(X)
#undef X
#define X(name, type) PagedVec name;
    ORE_NAMESPACES_PAGED_DIAG_COLUMNS(X)
    ORE_NAMESPACES_SLOT_COLUMNS(X)
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
  // No `meta` or `ref_count` column: visibility lives on NamespaceItem.meta
  // (the membership index the check driver's unused pass reads), and the old
  // impure ref_count was deleted with the resolve_ref that mutated it. A decl's
  // resolved meta is derivable from top_level_entry / namespace_items on demand.
#define ORE_DEFS_COLUMNS(X) \
    X(names,                StrId)         \
    X(parent_modules,       NamespaceId)   \
    X(kinds,                DefKind)       \
    X(kind_row,             uint32_t)      \
    X(identity_keys,        AstId)

// PagedVec-backed for the same UAF reason as ORE_FILES_PAGED_DIAG_COLUMNS:
// type_of_decl_sink_open captures &bundle->items / &bundle->args_arena
// interior pointers, which MUST survive db_create_def's Vec realloc on
// every other defs column during a held type_of_decl frame.
#define ORE_DEFS_PAGED_DIAG_COLUMNS(X)                                    \
    X(type_of_decl_diags,   DiagBundle)

// Per-decl preorder map of the decl WRAPPER subtree (signature + body
// for fns; whole decl for non-fns). Owned by TYPE_OF_DECL's compute
// path — refreshed on every recompute and consumed by emit-time
// span_of in type.c / infer.c. Resolved at publish time by a
// preorder walk over the current wrapper. See ast_id.h.
//
// Stored as a PagedVec keyed by DefId (1:1 with the def's row in
// defs.kinds). PagedVec because: (a) the rows hold a HashMap whose
// interior pointers (bucket array) must not move under realloc;
// (b) every def_idx is a valid index, including ones whose
// TYPE_OF_DECL hasn't run yet — the row holds an empty `{rev = {0},
// next_id = 0}` until first compute.
#define ORE_DEFS_PAGED_AST_ID_COLUMNS(X)                                  \
    X(decl_ast_id_maps,     DeclAstIdMap)
  struct {
#define X(name, type) Vec name;
    ORE_DEFS_COLUMNS(X)
#undef X
#define X(name, type) PagedVec name;
    ORE_DEFS_PAGED_DIAG_COLUMNS(X)
    ORE_DEFS_PAGED_AST_ID_COLUMNS(X)
#undef X
  } defs;

  // FUNCTION — db.fns. Per-query result columns:
  //   type             — TYPE_OF_DECL result (IpIndex; for fns this
  //                      delegates to FN_SIGNATURE's type field).
  //   signature_result — FN_SIGNATURE's full result (type + per-sig-
  //                      position node-types table). H11 folded the
  //                      former signature_node_types side column in here.
  //   body_node_types  — INFER_BODY's result (NodeTypesRange of per-
  //                      expression types in the body).
  //   body             — BODY_SCOPES' full result (scope-pool offsets +
  //                      embedded SyntaxNodePtr→ScopeId HashMap). H11
  //                      folded the former scope_map side column in here.
#define ORE_FNS_COLUMNS(X) \
    X(type,                  IpIndex)              \
    X(signature_result,      FnSignature)          \
    X(signature_shape_result, FnSignature)         \
    X(body_node_types,       NodeTypesRange)       \
    X(body_effect_row,       IpIndex)              \
    X(body,                  FnBody)               \
    X(fn_body_diags,         DiagBundle)           \
    X(signature_diags,       DiagBundle)           \
    X(slot_type_hot,         struct QuerySlotHot)  \
    X(slot_type_cold,        struct QuerySlotCold) \
    X(slot_signature_hot,    struct QuerySlotHot)  \
    X(slot_signature_cold,   struct QuerySlotCold) \
    X(slot_signature_shape_hot,  struct QuerySlotHot)  \
    X(slot_signature_shape_cold, struct QuerySlotCold) \
    X(slot_infer_hot,        struct QuerySlotHot)  \
    X(slot_infer_cold,       struct QuerySlotCold) \
    X(slot_body_scopes_hot,  struct QuerySlotHot)  \
    X(slot_body_scopes_cold, struct QuerySlotCold) \
    X(slot_body_references_hot,  struct QuerySlotHot)  \
    X(slot_body_references_cold, struct QuerySlotCold) \
    X(slot_decl_ast_map_hot,  struct QuerySlotHot)  \
    X(slot_decl_ast_map_cold, struct QuerySlotCold)
  struct {
#define X(name, type) PagedVec name;
    ORE_FNS_COLUMNS(X)
#undef X
  } fns;

  // STRUCT / UNION / ENUM / EFFECT / HANDLER / VARIABLE — uniform shape:
  // a cached QUERY_TYPE_OF_DECL result + its slot. Kept as distinct
  // tables (one per DefKind) so a by-kind scan stays dense. Field /
  // variant / param payloads are NOT duplicated here — they live in the
  // InternPool as part of the interned struct / enum / fn type.
  // STRUCT — db.structs.
  //   type_result.type        = TYPE_OF_DECL's IpIndex result.
  //   type_result.field_node_types = per-field annotation node types.
  // H11 folded both the former .type column AND the
  // .field_node_types side column into the single .type_result struct.
#define ORE_STRUCTS_COLUMNS(X) \
    X(type_result,      StructType)           \
    X(field_lo,         uint32_t)             \
    X(field_len,        uint32_t)             \
    X(slot_type_hot,    struct QuerySlotHot)  \
    X(slot_type_cold,   struct QuerySlotCold) \
    X(slot_decl_ast_map_hot,  struct QuerySlotHot)  \
    X(slot_decl_ast_map_cold, struct QuerySlotCold)
  struct {
#define X(name, type) PagedVec name;
    ORE_STRUCTS_COLUMNS(X)
#undef X
  } structs;

#define ORE_UNIONS_COLUMNS(X) \
    X(type,           IpIndex)              \
    X(field_lo,       uint32_t)             \
    X(field_len,      uint32_t)             \
    X(slot_type_hot,  struct QuerySlotHot)  \
    X(slot_type_cold, struct QuerySlotCold) \
    X(slot_decl_ast_map_hot,  struct QuerySlotHot)  \
    X(slot_decl_ast_map_cold, struct QuerySlotCold)
  struct {
#define X(name, type) PagedVec name;
    ORE_UNIONS_COLUMNS(X)
#undef X
  } unions;

#define ORE_ENUMS_COLUMNS(X) \
    X(type,           IpIndex)              \
    X(variant_lo,     uint32_t)             \
    X(variant_len,    uint32_t)             \
    X(slot_type_hot,  struct QuerySlotHot)  \
    X(slot_type_cold, struct QuerySlotCold) \
    X(slot_decl_ast_map_hot,  struct QuerySlotHot)  \
    X(slot_decl_ast_map_cold, struct QuerySlotCold)
  struct {
#define X(name, type) PagedVec name;
    ORE_ENUMS_COLUMNS(X)
#undef X
  } enums;

// Effects-4a — op_lo/op_len index db.aggregate_field_pool the same way
// structs/unions do. Each (StrId name → IpIndex fn_type) entry stamps an
// effect op; the fn_type's `effect_row` already carries the parent effect
// (injected via row_union at type_of_def time) so call-site accumulation
// is a single union into the body row.
#define ORE_EFFECTS_COLUMNS(X) \
    X(type,           IpIndex)              \
    X(op_lo,          uint32_t)             \
    X(op_len,         uint32_t)             \
    X(slot_type_hot,  struct QuerySlotHot)  \
    X(slot_type_cold, struct QuerySlotCold) \
    X(slot_decl_ast_map_hot,  struct QuerySlotHot)  \
    X(slot_decl_ast_map_cold, struct QuerySlotCold)
  struct {
#define X(name, type) PagedVec name;
    ORE_EFFECTS_COLUMNS(X)
#undef X
  } effects;

  // Slice 6.19 — KIND_DISTINCT: a thin table (type-cache cell + query slots).
  // NO field/variant pool — a distinct's only payload
  // is the backing IpIndex, which lives inside the interned IPK_DISTINCT_TYPE.
#define ORE_DISTINCTS_COLUMNS(X) \
    X(type,           IpIndex)              \
    X(slot_type_hot,  struct QuerySlotHot)  \
    X(slot_type_cold, struct QuerySlotCold) \
    X(slot_decl_ast_map_hot,  struct QuerySlotHot)  \
    X(slot_decl_ast_map_cold, struct QuerySlotCold)
  struct {
#define X(name, type) PagedVec name;
    ORE_DISTINCTS_COLUMNS(X)
#undef X
  } distincts;

  // VARIABLE — db.variables. type_result = full VariableType
  // (type + value_node_types). H11 folded both the former .type column
  // AND the value_node_types side column into the single .type_result.
#define ORE_VARIABLES_COLUMNS(X) \
    X(type_result,      VariableType)         \
    X(slot_type_hot,    struct QuerySlotHot)  \
    X(slot_type_cold,   struct QuerySlotCold) \
    X(slot_decl_ast_map_hot,  struct QuerySlotHot)  \
    X(slot_decl_ast_map_cold, struct QuerySlotCold)
  struct {
#define X(name, type) PagedVec name;
    ORE_VARIABLES_COLUMNS(X)
#undef X
  } variables;

  // CONSTANT — db.constants. type_result = full ConstantType
  // (type + value_node_types). H11 folded both the former .type column
  // AND the value_node_types side column into the single .type_result.
  // slot_const_eval is the (declared, not-yet-implemented)
  // QUERY_CONST_EVAL hook.
#define ORE_CONSTANTS_COLUMNS(X) \
    X(type_result,          ConstantType)         \
    X(slot_type_hot,        struct QuerySlotHot)  \
    X(slot_type_cold,       struct QuerySlotCold) \
    X(slot_const_eval_hot,  struct QuerySlotHot)  \
    X(slot_const_eval_cold, struct QuerySlotCold) \
    X(slot_decl_ast_map_hot,  struct QuerySlotHot)  \
    X(slot_decl_ast_map_cold, struct QuerySlotCold)
  struct {
#define X(name, type) PagedVec name;
    ORE_CONSTANTS_COLUMNS(X)
#undef X
  } constants;

  // HASHMAP-KEYED QUERIES — def_identity, resolve_ref.
  // Their slots live in dense Vec<QuerySlot> columns (like the per-kind
  // tables); db.def_by_identity / db.resolve_ref_cache route a packed
  // u64 key to a row index here.
  // Row 0 of each is a reserved sentinel; `results` holds the per-row
  // cached DefId. No `keys` column: def_identity's routing key
  // (nsid<<32 | astid.idx) is fully reversible, so recompute_DEF_IDENTITY
  // reconstructs (nsid, astid) straight from the key.
  struct {
    PagedVec results;    // PagedVec<DefId>
    PagedVec slots_hot;  // PagedVec<QuerySlotHot>
    PagedVec slots_cold; // PagedVec<QuerySlotCold>
    // G2: row-index free-list. engine_compact pushes each orphan row
    // after reclaim; db_query_slot_alloc pops here before paged_push
    // so a stranded PagedVec row gets reused instead of being leaked
    // for the db's lifetime. Without this, capacity grows monotonically
    // even though the slot/routing-map pair re-converges.
    Vec     free_rows;   // Vec<uint32_t>
  } def_identity;
  struct {
    PagedVec results;    // PagedVec<DefId>
    PagedVec slots_hot;  // PagedVec<QuerySlotHot>
    PagedVec slots_cold; // PagedVec<QuerySlotCold>
    Vec     free_rows;   // Vec<uint32_t> — G2 free-list (see def_identity)
  } resolve_ref;

  // QUERY_INFER_INSTANCE — monomorphization instances, HashMap-routed by
  // db.instances_cache from the interned IPK_INSTANCE IpIndex.v (a fully
  // reversible u64 key → no keys column; recompute reconstructs the
  // IpIndex straight from the key). results[row] holds the InstanceResult
  // (ret_type + per-instance body node-types). `diags` is a PARALLEL
  // DiagBundle column (mirror of fns.fn_body_diags) so a result overwrite
  // never clobbers the bundle; it rides the alloc_or_reuse_row `keys` slot.
  struct {
    PagedVec results;    // PagedVec<InstanceResult>
    PagedVec diags;      // PagedVec<DiagBundle> — per-instance body diags
    PagedVec slots_hot;  // PagedVec<QuerySlotHot>
    PagedVec slots_cold; // PagedVec<QuerySlotCold>
    Vec     free_rows;   // Vec<uint32_t> — G2 free-list (see def_identity)
  } instances;

  // QUERY_TOP_LEVEL_ENTRY — per-name reader over QUERY_NAMESPACE_ITEMS.
  // A dep-tracked pull query: it reads the namespace's items index to
  // resolve `name` to its AstId + current node_ptr, depends on
  // file_ast(item.file) to keep that ptr fresh, and emits a
  // position-independent content-hash fingerprint. Consumers
  // (def_identity, module_exports, resolve_ref) record per-name deps
  // here, so a sibling-decl edit re-runs this cheap reader but its stable
  // fp stops the cascade through the whole namespace.
  //
  // Routed by db.top_level_entry_cache from a packed
  // (nsid.idx << 32 | name.idx) key. results[row] holds the entry's
  // (id, name, node_ptr, meta) tuple. The keys column holds the original
  // StrId per row so a recompute thunk can recover the call args.
  //
  // NOT an enumeration source — keyed BY name, it can only answer about a
  // name you already hold. "What are the top-level items of namespace N?"
  // is QUERY_NAMESPACE_ITEMS (db.namespaces.items), which this reads.
  struct {
    PagedVec results;    // PagedVec<TopLevelEntry>
    PagedVec keys;       // PagedVec<StrId> — original name per row
    PagedVec slots_hot;  // PagedVec<QuerySlotHot>
    PagedVec slots_cold; // PagedVec<QuerySlotCold>
    Vec     free_rows;   // Vec<uint32_t> — G2 free-list (see def_identity)
  } top_level_entry;

  // Body-scope pools. db.fns.body[row] holds per-fn (off,len) ranges
  // into these two flat arrays (rust-analyzer ExprScopes, flattened).
  // The node→scope mapping is owned per-fn in db.fns.scope_map (HashMap<
  // syntax_node_ptr_hash, ScopeId>), not a dense shared pool.
  Vec body_scope_rows;   // Vec<ScopeRow>
  Vec body_scope_binds;  // Vec<ScopedBind>

  // Nominal field/variant pools (D2.1b). An aggregate's (struct OR union)
  // IpIndex is a stable inline identity (zir = def.idx); its member list lives
  // here, with db.structs.(field_lo,field_len) / db.unions.(field_lo,field_len)
  // the per-def range — the decl_pool pattern (recompute rewrites the range in
  // place, the pool compactor reclaims stranded ranges). Same for enums via
  // db.enums.(variant_lo, variant_len). Keeps recompute-churning data out of
  // the immutable intern pool.
  Vec aggregate_field_pool;  // Vec<AggregateFieldEntry> (structs + unions)
  Vec enum_variant_pool;     // Vec<EnumVariantEntry>

  // Effect operations get their OWN dedicated SoA — three parallel columns
  // indexed by db.effects.(op_lo + i), separate from aggregate_field_pool so
  // struct/union fields stay lean and a sort-only scan (linear check /
  // perform-shape) touches one byte per op. Same decl_pool pattern + a
  // dedicated compactor (compact_effect_op_pools). op_names/op_types together
  // are the op's {name, fn-type}; op_sorts is its OpSort.
  Vec effect_op_names;       // Vec<StrId>
  Vec effect_op_types;       // Vec<IpIndex>
  Vec effect_op_sorts;       // Vec<uint8_t> (OpSort)

  // Namespace export-member pool (D2.2). A namespace type's IpIndex is a
  // stable inline identity (nsid); its exported (name → DefId) member list
  // lives here, with db.namespaces.(field_lo, field_len) the per-namespace
  // range — same decl_pool pattern + compactor. A member is a name→def
  // binding (DeclEntry); member TYPES resolve lazily via type_of_def(def).
  Vec namespace_field_pool;  // Vec<DeclEntry>

  // Empty / cycle sentinel — a zero NodeTypesRange (uninitialized
  // HashMap). All lookups short-circuit to IP_NONE via
  // sema_node_types_range_lookup. Per-decl NodeTypesRanges own their
  // own HashMaps — no shared pool.
  NodeTypesRange empty_node_types_range;

  // SCOPES — fully X-macro driven. decl_lo / decl_len are per-scope
  // SoA columns indexing into the shared decl_pool: scope `i`'s decls
  // live at decl_pool[decl_lo[i] .. decl_lo[i] + decl_len[i]). Ranges
  // are independent of scope-id ordering — re-runs rewrite (lo, len)
  // in place without disturbing other scopes.
  // (Per-(scope, name) name-resolution slots live in db.resolve_ref,
  //  routed by db.resolve_ref_cache — many names per scope.)
#define ORE_SCOPES_COLUMNS(X)               \
    X(parents,        ScopeId)              \
    X(meta,           ScopeMeta)            \
    X(owning_modules, NamespaceId)          \
    X(decl_lo,        uint32_t)             \
    X(decl_len,       uint32_t)
  struct {
#define X(name, type) Vec name;
    ORE_SCOPES_COLUMNS(X)
#undef X
    Vec decl_pool;         // Vec<DeclEntry> — shared, append-only pool
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
  // QUERY_SOURCE_TEXT input slots — one per SourceId, Vec-indexed (dense),
  // grown in lockstep with the source columns. The slot's fingerprint is
  // the source content hash, set via db_input_set; readers (file_ast) dep
  // on it so verify invalidates them per-source.
#define ORE_SOURCES_SLOT_COLUMNS(X)         \
    X(slots_text_hot,  struct QuerySlotHot) \
    X(slots_text_cold, struct QuerySlotCold)
  struct {
#define X(name, type) Vec name;
    ORE_SOURCES_COLUMNS(X)
#undef X
#define X(name, type) PagedVec name;
    ORE_SOURCES_SLOT_COLUMNS(X)
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
  HashMap virtual_by_name;     // interned synthetic-name StrId.idx → SourceId.idx
  HashMap def_by_identity;     // (nsid.idx<<32 | ast_id.idx) → db.def_identity row
  HashMap resolve_ref_cache;   // (scope.idx<<32 | name.idx) → db.resolve_ref row
  HashMap comptime_call_cache;
  HashMap top_level_entry_cache; // (nsid.idx<<32 | name.idx) → db.top_level_entry row
  HashMap instances_cache;     // interned IPK_INSTANCE IpIndex.v → db.instances row

  // Phase P cutover — diagnostics live in per-query-result DiagBundle
  // columns (db.files.parse_diags, db.fns.fn_body_diags, etc.). The
  // legacy centralized DiagList side-channel was deleted; no field needed.
};

void db_init(struct db *s);
void db_free(struct db *s);

// Synthetic-primitive helpers — see db.c for the architectural notes.
//   db_primitive_type_for(d)        returns the IpIndex constant for a
//                                    primitive DefId; IP_NONE otherwise.
//   db_is_primitives_scope(s, sc)   true iff `sc` is the synthetic
//                                    parent-scope of every namespace.
//   db_primitive_def_for_slot(s, i) maps a decl_pool index inside the
//                                    primitives scope to its DefId
//                                    (used by resolve_ref's hit-branch
//                                    short-circuit).
IpIndex db_primitive_type_for(struct db *s, DefId def);
bool    db_is_primitives_scope(struct db *s, ScopeId scope);
DefId   db_primitive_def_for_slot(struct db *s, uint32_t slot_in_pool);

// Mark-and-copy compaction across the shared pools
// (body_scope_rows + binds, decl_pool).
// Each pool checks `count > last_compacted * GROWTH_FACTOR && count >
// MIN_THRESHOLD` and runs compaction if so. Reclaims pool entries that
// became unreachable when their owning query re-ran. Called from
// db_request_end — the canonical safe point where no Vec.data raw
// pointer survives across the call.
void db_pools_maybe_compact(struct db *s);

// --- Per-DefId routing -------------------------------------------------
//
// kinds[d] selects the per-kind table; kind_row[d] indexes every column
// of it. All per-kind column access (db.fns.*, db.structs.*, …) routes
// through db_def_row, which asserts the def is classified to `want`.
static inline DefKind db_def_kind(struct db *s, DefId d) {
  return *(DefKind *)vec_get(&s->defs.kinds, d.idx);
}
// S1 — content-addressed DeclKey for body-anchored diags. Stashed at
// def-creation time in db_query_def_identity; the value IS the AstId
// computed via ast_id_compute(kind, name), so it survives reparses
// that preserve a decl's (kind, name) identity.
static inline AstId db_def_decl_key(struct db *s, DefId d) {
  return *(AstId *)vec_get(&s->defs.identity_keys, d.idx);
}
static inline uint32_t db_def_row(struct db *s, DefId d, DefKind want) {
  assert(db_def_kind(s, d) == want && "db_def_row: def kind mismatch");
  (void)want;
  return *(uint32_t *)vec_get(&s->defs.kind_row, d.idx);
}

// The QUERY_TYPE_OF_DECL result cell for a def. For kinds with no side
// data (FN/UNION/ENUM/EFFECT/HANDLER), it's the per-kind `type` column.
// For kinds whose result struct embeds side data (STRUCT/VARIABLE/
// CONSTANT after H11), the IpIndex lives at `type_result.type` — same
// memoized value, different storage location. Pointer-stability caveat
// applies (do not hold across a per-kind Vec grow).
static inline IpIndex *db_def_type_cell(struct db *s, DefId d) {
  uint32_t row = *(uint32_t *)vec_get(&s->defs.kind_row, d.idx);
  switch (db_def_kind(s, d)) {
  case KIND_FUNCTION: return (IpIndex *)paged_get(&s->fns.type, row);
  case KIND_STRUCT:   return &((StructType   *)paged_get(&s->structs.type_result,   row))->type;
  case KIND_UNION:    return (IpIndex *)paged_get(&s->unions.type, row);
  case KIND_ENUM:     return (IpIndex *)paged_get(&s->enums.type, row);
  case KIND_EFFECT:   return (IpIndex *)paged_get(&s->effects.type, row);
  case KIND_DISTINCT: return (IpIndex *)paged_get(&s->distincts.type, row);
  case KIND_VARIABLE: return &((VariableType *)paged_get(&s->variables.type_result, row))->type;
  case KIND_CONSTANT: return &((ConstantType *)paged_get(&s->constants.type_result, row))->type;
  default: break;
  }
  assert(0 && "db_def_type_cell: unclassified def");
  return NULL;
}

// The QUERY_FN_SIGNATURE result cell — db.fns.signature_result. Asserts
// the def is a function. Returns a pointer to the full FnSignature
// struct (type + node_types). Pointer-stable post-PagedVec migration.
static inline FnSignature *db_fn_signature_cell(struct db *s, DefId d) {
  return (FnSignature *)paged_get(&s->fns.signature_result,
                                  db_def_row(s, d, KIND_FUNCTION));
}

// Aggregate (struct|union) member access. These NEVER hand out a pointer into
// the aggregate_field_pool: the pool is a growable Vec (a later type_of_def
// append can realloc it) that the pool compactor also relocates, so a borrowed
// pointer would be a footgun. Instead members are returned BY VALUE, with the
// range base re-derived each call — robust across pool growth and compaction.
// `db_aggregate_field_count` returns 0 for non-aggregate kinds.
//
// Consumers reading these must still record a dep on db_query_type_of_def(d)
// so a field-content edit (which flips that query's fp) invalidates them — the
// getters are raw reads, like db_get_namespace_files.
static inline uint32_t db_aggregate_field_count(struct db *s, DefId d) {
  DefKind k = db_def_kind(s, d);
  PagedVec *len_col;
  if (k == KIND_STRUCT)     len_col = &s->structs.field_len;
  else if (k == KIND_UNION) len_col = &s->unions.field_len;
  else                      return 0;
  uint32_t row = *(uint32_t *)vec_get(&s->defs.kind_row, d.idx);
  return *(uint32_t *)paged_get(len_col, row);
}

// The i-th member, by value. Precondition: i < db_aggregate_field_count(s, d)
// (so d is a struct/union). The range base is re-read each call, so growth /
// compaction between calls is harmless.
static inline AggregateFieldEntry db_aggregate_field_at(struct db *s, DefId d,
                                                        uint32_t i) {
  PagedVec *lo_col = (db_def_kind(s, d) == KIND_STRUCT) ? &s->structs.field_lo
                                                        : &s->unions.field_lo;
  uint32_t row = *(uint32_t *)vec_get(&s->defs.kind_row, d.idx);
  uint32_t lo  = *(uint32_t *)paged_get(lo_col, row);
  return *(AggregateFieldEntry *)vec_get(&s->aggregate_field_pool, lo + i);
}

// Type of member `name` in a struct/union, or IP_NONE if absent — the canonical
// field-access lookup, scanning the range internally (no pool pointer escapes).
static inline IpIndex db_aggregate_field_type(struct db *s, DefId d, StrId name) {
  uint32_t n = db_aggregate_field_count(s, d);
  for (uint32_t i = 0; i < n; i++) {
    AggregateFieldEntry e = db_aggregate_field_at(s, d, i);
    if (e.name.idx == name.idx) return e.type;
  }
  return IP_NONE;
}

// Effects-4a — an effect decl's op list (name → fn_type). Each entry's
// fn_type carries the parent effect baked into its effect_row (injected
// via row_union at type_of_def time). Same recompute-friendly pool shape
// as struct/union fields; `op_lo/op_len` index db.aggregate_field_pool.
//
// Consumers must record a dep on db_query_type_of_def(d) — the pool is a
// raw read just like db_aggregate_field_count.
static inline uint32_t db_effect_op_count(struct db *s, DefId d) {
  if (db_def_kind(s, d) != KIND_EFFECT) return 0;
  uint32_t row = *(uint32_t *)vec_get(&s->defs.kind_row, d.idx);
  return *(uint32_t *)paged_get(&s->effects.op_len, row);
}

// The op's {name, fn-type} synthesized from the parallel columns (API-stable
// for callers that want both; sort-only callers use db_effect_op_sort).
static inline AggregateFieldEntry db_effect_op_at(struct db *s, DefId d,
                                                  uint32_t i) {
  uint32_t row = *(uint32_t *)vec_get(&s->defs.kind_row, d.idx);
  uint32_t lo  = *(uint32_t *)paged_get(&s->effects.op_lo, row);
  return (AggregateFieldEntry){
      .name = *(StrId *)vec_get(&s->effect_op_names, lo + i),
      .type = *(IpIndex *)vec_get(&s->effect_op_types, lo + i)};
}

// The op's declared sort (drives sort-compat + perform shape).
static inline OpSort db_effect_op_sort(struct db *s, DefId d, uint32_t i) {
  uint32_t row = *(uint32_t *)vec_get(&s->defs.kind_row, d.idx);
  uint32_t lo  = *(uint32_t *)paged_get(&s->effects.op_lo, row);
  return (OpSort) * (uint8_t *)vec_get(&s->effect_op_sorts, lo + i);
}

// Type of op `name` on an effect decl, or IP_NONE if absent. Name scan touches
// only effect_op_names; the matched type comes from effect_op_types.
static inline IpIndex db_effect_op_type(struct db *s, DefId d, StrId name) {
  uint32_t row = *(uint32_t *)vec_get(&s->defs.kind_row, d.idx);
  uint32_t lo  = *(uint32_t *)paged_get(&s->effects.op_lo, row);
  uint32_t n   = db_effect_op_count(s, d);
  for (uint32_t i = 0; i < n; i++) {
    StrId nm = *(StrId *)vec_get(&s->effect_op_names, lo + i);
    if (nm.idx == name.idx)
      return *(IpIndex *)vec_get(&s->effect_op_types, lo + i);
  }
  return IP_NONE;
}

// Index of op `name` on an effect decl, or UINT32_MAX if absent. Companion to
// db_effect_op_type for callers that need the op's position/sort (handler
// clause validation): resolve name→index here, then db_effect_op_sort(s,d,idx).
static inline uint32_t db_effect_op_index(struct db *s, DefId d, StrId name) {
  uint32_t row = *(uint32_t *)vec_get(&s->defs.kind_row, d.idx);
  uint32_t lo  = *(uint32_t *)paged_get(&s->effects.op_lo, row);
  uint32_t n   = db_effect_op_count(s, d);
  for (uint32_t i = 0; i < n; i++) {
    StrId nm = *(StrId *)vec_get(&s->effect_op_names, lo + i);
    if (nm.idx == name.idx)
      return i;
  }
  return UINT32_MAX;
}

// A nominal enum's resolved variant list (name → value), recovered from
// db.enum_variant_pool via db.enums.(variant_lo, variant_len). Same contract.
static inline const EnumVariantEntry *db_enum_variants(struct db *s, DefId d,
                                                       uint32_t *len_out) {
  *len_out = 0;
  if (db_def_kind(s, d) != KIND_ENUM) return NULL;
  uint32_t row = *(uint32_t *)vec_get(&s->defs.kind_row, d.idx);
  uint32_t lo  = *(uint32_t *)paged_get(&s->enums.variant_lo,  row);
  uint32_t len = *(uint32_t *)paged_get(&s->enums.variant_len, row);
  *len_out = len;
  return len ? (const EnumVariantEntry *)vec_get(&s->enum_variant_pool, lo)
             : NULL;
}

// A namespace type's exported members (name → DefId), recovered from
// db.namespace_field_pool via db.namespaces.(field_lo, field_len). By value,
// no pool pointer escapes (same contract as db_aggregate_field_at). Member
// TYPES are lazy: call db_query_type_of_def(member.def).
static inline uint32_t db_namespace_member_count(struct db *s, NamespaceId n) {
  if (n.idx >= s->namespaces.field_len.count) return 0;
  return *(uint32_t *)vec_get(&s->namespaces.field_len, n.idx);
}
static inline DeclEntry db_namespace_member_at(struct db *s, NamespaceId n,
                                               uint32_t i) {
  uint32_t lo = *(uint32_t *)vec_get(&s->namespaces.field_lo, n.idx);
  return *(DeclEntry *)vec_get(&s->namespace_field_pool, lo + i);
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
// Bodies live in src/db/inputs/<entity>.c. The query engine writes
// query results internally; that is NOT the input boundary.
// =============================================================================

// --- Inputs: source ----------------------------------------------------------
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

// --- Inputs: file ------------------------------------------------------------
FileId   db_create_file(struct db *s, SourceId src, NamespaceId owner);
// Lazy-load variant for physical files: skips the DUR_MEDIUM revision
// bump db_create_file does — owner MUST be a freshly-created namespace
// (no prior queries to invalidate). Used by workspace_resolve_import to
// admit an @import target from inside infer_body's request frame; the
// bump would be a structural no-op (nothing depends on an empty FILE_SET)
// but would trip db_input_changed's "no open request" assert.
FileId   db_create_file_lazy(struct db *s, SourceId src, NamespaceId owner);
// Virtual-file allocation: FileId has the virtual bit set; skips the
// DUR_MEDIUM revision bump db_create_file does (the owner is expected to
// be a freshly-created namespace, so there are no prior queries to
// invalidate) but still folds into FILE_SET / member_files. Pair with
// db_admit_virtual_source.
FileId   db_create_virtual_file(struct db *s, SourceId src, NamespaceId owner);
// Remove a file from its namespace's membership on eviction: drops it from
// the per-namespace reverse index and recomputes the FILE_SET fingerprint
// from the survivors. Caller provides the revision bump.
void     db_namespace_remove_file(struct db *s, NamespaceId owner, FileId fid);
// L1 — readmit a previously-evicted source: clears the `evicted` bit and
// re-joins its file to the owning namespace's membership (file_set_add
// path). No-op if the source isn't evicted, so safe to call on every
// workspace_did_open / workspace_did_change_external. Without this, a
// reopen after eviction leaves the namespace permanently empty.
void     db_readmit_source(struct db *s, SourceId src);

// --- Inputs: module ----------------------------------------------------------
// Allocate a new module row. File-as-namespace model: every file gets
// its own fresh NamespaceId; sibling files do NOT share a module.
NamespaceId db_create_namespace(struct db *s);

// --- Inputs: diag (emit into the active query slot) --------------------------
//   Declared in src/db/diag/diag.h (db_emit_error, _warning, _info, _hint,
//   plus typed variants _c / _s / _n / _t / _ss / _va).
//   The header lives with the Diag/DiagSeverity types; the bodies live in
//   src/db/inputs/diag.c.

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
// db_get_file_ast / db_get_file_ast_id_map removed in Phase 3
// (flat-AST gone). Callers now read files.green_roots[fid] directly
// for the GreenNode, then use src/syntax navigation.

// --- Getters: module ---------------------------------------------------------
const FileId *db_get_namespace_files(struct db *s, NamespaceId nsid,
                                  uint32_t *out_count);

// --- Getters: diag -----------------------------------------------------------
//   db_collect_diags_for_file, db_format_diag, db_resolve_span, and the
//   DiagResolver bulk-render API (diag_resolver_init/free/resolve/print)
//   are declared in src/db/diag/diag.h alongside the Diag /
//   DiagAnchor / ResolvedSpan types.

// --- Getters: type rendering -------------------------------------------------
size_t db_format_type(struct db *s, IpIndex t, char *buf, size_t cap);

// --- Getters: position / cursor (LSP, CLI --at-position) ---------------------
uint32_t  db_byte_offset_at(struct db *s, FileId fid,
                            uint32_t line0, uint32_t char0);
// Resolves to the innermost SyntaxNode covering `byte_offset` in
// files.green_roots[fid]. RETURNS_OWNED — caller must release via
// syntax_node_release. Returns NULL for invalid file or offset.
SyntaxNode *db_node_at_offset(struct db *s, FileId fid, uint32_t byte_offset);

#endif
