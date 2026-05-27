#ifndef ORE_DB_QUERY_ENGINE_H
#define ORE_DB_QUERY_ENGINE_H

// ============================================================================
// Query engine — public API
//
// Salsa-style memoized derivation engine. Per-entry granularity native.
// Push-stamp model: source-of-truth queries (file_ast) write per-entry
// outputs directly into derived slots; consumers read pre-stamped slots
// without re-deriving.
//
// Design commitments — see plan §"Architectural commitments":
//   - Per-entry queries native (no whole-namespace/whole-file fingerprints
//     leaking into per-decl deps)
//   - Pure query model (results flow through return values, never via
//     side-effect writes into shared SoA)
//   - Push-stamp is first-class (db_query_stamp_direct in internal.h)
//   - Push-stamp liveness drives orphan reclamation (a slot whose
//     verified_rev falls behind the current revision is orphan)
//   - Internal vs external API separation (engine_internal.h holds the
//     primitives only the engine + parse layer may use)
//   - Hashmap dep dedup by construction (no linear scans on hot path)
//   - Cycle handling unified through DB_QUERY_GUARD's on_cycle arg
//   - Cancellation honored by the guard (early-return on CANCELED)
//   - Compile-time dispatch exhaustiveness (X-macro over QueryKind)
//   - Per-input durability (deps record their own tier; verify-time MIN)
//   - Production telemetry (counters always on, not debug-gated)
//
// This header is included by layer files (parse.c, scope.c, type.c,
// diag.c) and by callers outside src/db/query/ that need to invoke
// queries. It must NOT pull in internal-only primitives — those live
// in engine_internal.h.
// ============================================================================

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// ----------------------------------------------------------------------------
// db_query_ctx — the engine's handle type
//
// Today: typedef alias for `struct db`. Engine API and consumer code
// are wire-compatible at the C type level.
//
// Tomorrow (when parallel queries arrive): replace this typedef with
// `typedef struct db_query_ctx_s db_query_ctx;` — a real struct holding
// a `db *` plus per-thread state (in-flight stack, frame arena, cancel
// flag). A `db_query_ctx_new(struct db *)` constructor materializes it.
// Every engine API call already takes `db_query_ctx *`, so existing
// callers compile unchanged once a ctx is acquired.
//
// This split exists to migration-proof the engine API. Single-mutator
// today; per-thread later without breaking signatures.
// ----------------------------------------------------------------------------

struct db;
typedef struct db db_query_ctx;

// ----------------------------------------------------------------------------
// QueryKind enumeration
//
// Single source of truth via the X-macro. The macro generates BOTH the
// enum and the dispatch table; a new kind without a thunk is a link-time
// error (engine_dispatch.c).
//
// Dead/scaffold-only kinds from the previous architecture (LAYOUT_OF_TYPE,
// INSTANTIATE_DECL, EFFECT_SIG, BODY_EFFECTS, MODULE_AST, SCOPE_FOR_NODE,
// SCOPE_DECLS, SCOPE_PARENT, EFFECT_OPS_VISIBLE, FN_SCOPE_INDEX,
// STRUCT_SIGNATURE, ENUM_SIGNATURE, IS_COMPTIME, BODY_STORE, RESOLVE_PATH)
// are NOT in this list. If they become real queries later, add them here.
// ----------------------------------------------------------------------------

// 13 active kinds. No aggregating index queries — consumers that need
// "all entries in a namespace" iterate top_level_entry slots directly
// and record per-entry deps. This is the per-entry contract: no slot
// has whole-namespace granularity in either its fingerprint or its
// dep relationships.
#define ORE_QUERY_KINDS(X)                                                     \
    /* Parse layer */                                                          \
    X(FILE_AST)              /* whole-file parse → green tree + push-stamps */ \
    X(DECL_AST)              /* per-decl green subtree handle */               \
    X(FILE_IMPORTS)          /* per-file @import refs */                       \
    /* Scope / name layer */                                                   \
    X(TOP_LEVEL_ENTRY)       /* per-name top-level entry in a namespace */     \
    X(NAMESPACE_SCOPES)      /* internal + exported scopes for a namespace */  \
    X(DEF_IDENTITY)          /* canonical DefId for (namespace, ptr) */        \
    X(NODE_TO_DEF)           /* file's SyntaxNodePtr → DefId map */            \
    X(RESOLVE_REF)           /* name lookup in a scope */                      \
    /* Type layer */                                                           \
    X(TYPE_OF_DECL)          /* a decl's overall type (IpIndex) */             \
    X(FN_SIGNATURE)          /* fn-only: parameter + return types */           \
    X(INFER_BODY)            /* fn-only: body type-check */                    \
    X(BODY_SCOPES)           /* fn-only: lexical scopes within a body */       \
    X(NAMESPACE_TYPE)        /* IPK_NAMESPACE_TYPE for a namespace */

typedef enum {
#define X(name) QUERY_##name,
    ORE_QUERY_KINDS(X)
#undef X
    QUERY_KIND_COUNT
} QueryKind;

// Human-readable name. Used in panics, traces, telemetry dumps.
const char *db_query_kind_name(QueryKind kind);

// ----------------------------------------------------------------------------
// Result of db_query_begin
//
// COMPUTE  — caller's body must run, then call db_query_succeed or _fail.
// CACHED   — cached value is valid for this revision; no recomputation.
//            DB_QUERY_GUARD returns the cached-value sentinel and skips
//            the body entirely.
// CYCLE    — recursive entry into a RUNNING slot; the cycling call must
//            return without computing. DB_QUERY_GUARD returns on_cycle.
// ERROR    — slot is cached as a failure; consumer treats this as "no
//            value, but no re-run needed."
// CANCELED — cancellation was requested; the engine returns immediately.
//            DB_QUERY_GUARD returns on_cycle (treat as "can't compute now").
// ----------------------------------------------------------------------------

typedef enum {
    QUERY_BEGIN_COMPUTE,
    QUERY_BEGIN_CACHED,
    QUERY_BEGIN_CYCLE,
    QUERY_BEGIN_ERROR,
    QUERY_BEGIN_CANCELED,
} QueryBeginResult;

typedef uint64_t Fingerprint;
#define FINGERPRINT_NONE ((Fingerprint)0)

// Fingerprint construction. Layer code uses these to fold its query's
// semantic inputs into a single u64. FNV-style: associative, fast,
// deterministic. FINGERPRINT_NONE (== 0) is reserved — db_fp_u64
// remaps an incoming 0 to a non-zero canonical value so a real
// fingerprint never collides with the sentinel.
Fingerprint db_fp_u64(uint64_t x);
Fingerprint db_fp_combine(Fingerprint a, Fingerprint b);
Fingerprint db_fp_bytes(const void *p, size_t n);

// ----------------------------------------------------------------------------
// Slot lifecycle
//
// Every query body uses DB_QUERY_GUARD; manual handling of states is an
// anti-pattern. CANCELED is treated as CYCLE (caller's natural "can't
// compute" sentinel).
// ----------------------------------------------------------------------------

QueryBeginResult db_query_begin(db_query_ctx *ctx, QueryKind kind, uint64_t key);
void             db_query_succeed(db_query_ctx *ctx, QueryKind kind,
                                  uint64_t key, Fingerprint fp);
void             db_query_fail(db_query_ctx *ctx, QueryKind kind, uint64_t key);

// Ensure (kind, key) is computed (cached or recomputed) but do NOT
// record it as a dep on the calling query. For preconditions that
// don't carry a semantic dep — rare under the pure query model; kept
// as a fallback for adapter code.
void             db_query_ensure(db_query_ctx *ctx, QueryKind kind, uint64_t key);

#define DB_QUERY_GUARD(s, kind, key, on_cached, on_cycle, on_error)            \
    do {                                                                       \
        QueryBeginResult __r = db_query_begin((s), (kind), (key));             \
        if (__r == QUERY_BEGIN_CACHED)   return (on_cached);                   \
        if (__r == QUERY_BEGIN_CYCLE)    return (on_cycle);                    \
        if (__r == QUERY_BEGIN_ERROR)    return (on_error);                    \
        if (__r == QUERY_BEGIN_CANCELED) return (on_cycle);                    \
    } while (0)

// ----------------------------------------------------------------------------
// Introspection
//
// First-class observation API. Single surface, multiple consumers:
//   - Layer code (e.g., diag collection filters by db_slot_is_live)
//   - Tests (verify state machine semantics)
//   - LSP profilers + debug extensions (future)
//   - Telemetry exporters (future)
//
// Two scopes:
//   - Aggregate per-kind counters (QueryStats)
//   - Per-slot state probes (db_slot_*)
//
// Future additions (timing per-event, slot enumeration, dep-graph
// readback) build on the same primitives — no breaking-change pressure
// on existing consumers.
//
// Always-on (not gated on debug builds). Cost: O(QUERY_KIND_COUNT)
// counter storage + per-slot reads as cheap as a routed slot lookup.
// ----------------------------------------------------------------------------

typedef enum {
    QUERY_EMPTY = 0,
    QUERY_RUNNING,
    QUERY_DONE,
    QUERY_ERROR,
} QueryState;

// Liveness: true iff the slot is DONE and was verified/stamped at the
// current request revision. Orphan slots (DONE but verified_rev <
// current) return false — their data is conceptually stale and gets
// reclaimed by lazy GC. Diag collection filters by this primitive.
bool        db_slot_is_live(db_query_ctx *ctx, QueryKind kind, uint64_t key);

// Per-slot state probes. Hot fields read first (single SoA column
// touch); cold field (computed_rev) walks a parallel column.
QueryState  db_slot_state(db_query_ctx *ctx, QueryKind kind, uint64_t key);
Fingerprint db_slot_fingerprint(db_query_ctx *ctx, QueryKind kind, uint64_t key);
uint64_t    db_slot_verified_rev(db_query_ctx *ctx, QueryKind kind, uint64_t key);
uint64_t    db_slot_computed_rev(db_query_ctx *ctx, QueryKind kind, uint64_t key);

// Aggregate per-kind counters. Reset is total — clears all kinds.
// Useful for benchmarking ("how many begins between request N and N+1").
typedef struct {
    uint64_t begin;             // total db_query_begin calls
    uint64_t cached_hit;        // returned CACHED or ERROR without re-run
    uint64_t compute;           // returned COMPUTE → body ran
    uint64_t cycle;             // returned CYCLE
    uint64_t error;             // db_query_fail calls
    uint64_t orphan_reclaimed;  // slots GC'd at compaction
} QueryStats;

QueryStats  db_query_stats(db_query_ctx *ctx, QueryKind kind);
void        db_query_stats_reset(db_query_ctx *ctx);

// ----------------------------------------------------------------------------
// Inputs + revisions
//
// Inputs are external state the engine doesn't memoize (file text, file
// existence, workspace membership). Setters in src/db/setters/ call
// db_input_changed to bump the global revision; the engine's verify
// fast-path uses per-durability "last changed at" markers to skip the
// dep walk when no input at the slot's tier has moved.
// ----------------------------------------------------------------------------

typedef enum : uint8_t {
    DUR_LOW    = 0,  // workspace file text — keystroke-frequent
    DUR_MEDIUM = 1,  // file-set / module-set — edit-session-frequent
    DUR_HIGH   = 2,  // library sources — effectively immutable per session
} Durability;
#define DUR_COUNT 3

uint64_t db_input_changed(db_query_ctx *ctx, Durability dur);
uint64_t db_current_revision(db_query_ctx *ctx);
uint64_t db_effective_revision(db_query_ctx *ctx);

// Declare that the running body read an input of the given durability.
// Lowers the frame's MIN-durability accumulator. Idempotent.
void     db_query_note_input_durability(db_query_ctx *ctx, Durability dur);

// ----------------------------------------------------------------------------
// Request scoping
//
// A request pins effective_revision so a query plan sees a consistent
// snapshot of inputs. Outside a request, query results are undefined.
// ----------------------------------------------------------------------------

void db_request_begin(db_query_ctx *ctx, uint64_t revision);
void db_request_end(db_query_ctx *ctx);
void db_request_cancel(db_query_ctx *ctx);
bool db_check_cancel(db_query_ctx *ctx);

// ----------------------------------------------------------------------------
// Frame stack — exposed for diag emission (which routes by frame top).
// Layer code shouldn't reach for this; the engine state-machine handles
// most needs through DB_QUERY_GUARD.
// ----------------------------------------------------------------------------

typedef struct QueryFrame QueryFrame;
QueryFrame *db_query_stack_top(db_query_ctx *ctx);

// Frame field accessors (engine controls the struct layout).
QueryKind   db_frame_kind(const QueryFrame *f);
uint64_t    db_frame_key(const QueryFrame *f);

#endif // ORE_DB_QUERY_ENGINE_H
