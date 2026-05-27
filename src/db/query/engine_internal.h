#ifndef ORE_DB_QUERY_ENGINE_INTERNAL_H
#define ORE_DB_QUERY_ENGINE_INTERNAL_H

// ============================================================================
// Query engine — INTERNAL API
//
// Privileged primitives. Includers MUST be in one of these allowed sets:
//   - src/db/query/engine_*.c    (the engine implementation itself)
//   - src/db/query/parse.c       (the parse layer, which push-stamps)
//   - src/db/db.c                (db_init for engine initialization)
//
// No other file may include this header. The engine's contract with
// the rest of the codebase is via engine.h; this file holds the levers
// that would be foot-guns in layer code (writing slot state directly,
// bypassing dep tracking, reclaiming slots out-of-band).
//
// Anything declared here is intentionally not in engine.h. The split is
// the engine-enforced contract — adding a primitive to this header is
// a deliberate decision, never a convenience.
// ============================================================================

#include <stdbool.h>
#include <stdint.h>

#include "../../support/data_structure/hashmap.h"
#include "../../support/data_structure/vec.h"
#include "engine.h"

// ----------------------------------------------------------------------------
// Slot data — engine controls layout
//
// Two parallel SoA columns per kind:
//   hot  — touched on every dep walk + verify; one cache line per slot.
//   cold — lifecycle bookkeeping (computed_rev, last-stamped diagnostic
//          source); read by tests + introspection.
//
// `verified_rev == db_current_revision(ctx)` is the liveness predicate.
// Push-stamping a slot sets its verified_rev to the current revision
// without going through the begin/succeed dance.
// ----------------------------------------------------------------------------

typedef struct QuerySlotHot QuerySlotHot;
typedef struct QuerySlotCold QuerySlotCold;
typedef struct QueryFrame QueryFrame;

struct QuerySlotHot {
    QueryState  state;           // EMPTY / RUNNING / DONE / ERROR
    QueryKind   kind;            // self-identifying for diagnostics
    uint8_t     has_untracked_read;
    Durability  durability;      // cached MIN-at-succeed over (deps' dep_dur,
                                 //   noted-input durabilities). Used by the
                                 //   verify fast-path:
                                 //     dur_last_changed[slot.durability] <=
                                 //     slot.verified_rev → skip dep walk.
                                 //
                                 // QueryDep also carries dep_dur per dep —
                                 // recorded for Phase 8 upgrade to
                                 // "verify-time MIN over live deps" if/when
                                 // profiling demands finer granularity.
    Fingerprint fingerprint;     // memoized result fp (FINGERPRINT_NONE if EMPTY)
    uint64_t    verified_rev;    // last revision proven current
    Vec        *deps;            // *Vec<QueryDep>, lazy-allocated in db.arena
    HashMap    *dep_index;       // *HashMap<(kind<<32|key_lo), dep_row>
                                 // for O(1) dedup during record. lazy-alloc.
};

struct QuerySlotCold {
    uint64_t computed_rev;        // last revision this slot's body executed
    uint64_t stamped_rev;         // last revision via stamp_direct (0 if never)
};

// ----------------------------------------------------------------------------
// Dep record + frame
//
// QueryDep carries per-input durability (NEW vs old engine's MIN-at-
// succeed): verify-time can compute MIN over LIVE deps, not over the
// snapshot taken when the slot last succeeded.
// ----------------------------------------------------------------------------

typedef struct {
    uint64_t    key;
    Fingerprint dep_fp;
    QueryKind   kind;
    Durability  dep_dur;          // per-input durability — used at verify time
    uint8_t     _pad[2];
} QueryDep;

struct QueryFrame {
    QueryKind kind;
    uint64_t  key;
    Vec      *deps;               // arena-allocated; same buffer as slot->deps
    HashMap  *dep_index;          // arena-allocated; mirrors slot->dep_index
    Durability min_input_dur;
    bool       dur_set;
    uint8_t    _pad[6];
};

// Frame accessors (engine.h declares these; defined in engine.c).
// QueryKind   db_frame_kind(const QueryFrame *f);
// uint64_t    db_frame_key(const QueryFrame *f);

// ----------------------------------------------------------------------------
// Push-stamp — the load-bearing primitive
//
// Sets a slot's fingerprint + verified_rev to (fp, db_current_revision(ctx))
// in a single atomic-from-the-caller's-PoV step. State transitions to
// QUERY_DONE if it was EMPTY or DONE; if RUNNING (in flight) or ERROR,
// the call is rejected via assert — a producer cannot retroactively
// overwrite a slot that's in flight or cached-errored without going
// through the normal cycle.
//
// Used by file_ast: walks the parsed tree, computes per-decl content
// hashes, calls stamp_direct(DECL_AST, key, fp) and stamp_direct(
// TOP_LEVEL_ENTRY, key, fp) for each entry it found. Consumers later
// querying those slots cache-hit trivially because the slot is already
// DONE at the current revision.
//
// Deps semantics: stamp_direct does NOT record the caller as a dep,
// nor does it consume the slot's old deps. The slot is treated as
// "owned by the stamping producer" — its deps come from whoever PUSHED
// to it, not whoever last computed it as a pull-derived query.
//
// Liveness: a stamped slot's verified_rev == current_rev, so it's live.
// A slot NOT stamped at the current revision has verified_rev < current
// → orphan → eligible for reclamation.
// ----------------------------------------------------------------------------

void db_query_stamp_direct(db_query_ctx *ctx, QueryKind kind, uint64_t key,
                           Fingerprint fp);

// ----------------------------------------------------------------------------
// Slot allocation
//
// Pre-allocates a slot row in the appropriate SoA column without
// transitioning it. Used by file_ast when it discovers an entry it's
// about to stamp. Returns a stable row index; subsequent locate_slot
// calls for the same (kind, key) return the same row.
//
// For HashMap-routed kinds (TOP_LEVEL_ENTRY, DEF_IDENTITY, RESOLVE_REF,
// DECL_AST), this inserts the routing entry. For Vec-indexed kinds
// (FILE_AST keyed by FileId, etc.), this grows the column to fit.
// ----------------------------------------------------------------------------

void db_query_slot_alloc(db_query_ctx *ctx, QueryKind kind, uint64_t key);

// ----------------------------------------------------------------------------
// Orphan reclamation
//
// Called from engine_compact.c during db_request_end's compaction pass.
// Walks every kind's slot table; any slot with verified_rev < threshold
// is reclaimed:
//   - state ← QUERY_EMPTY
//   - fingerprint ← FINGERPRINT_NONE
//   - deps + dep_index Vecs zeroed (returned to a free list if backed
//     by malloc; arena-backed structs just zero the pointers)
//   - associated diag unit cleared (db_diags_clear)
//   - HashMap-routed kinds: routing entry removed
//
// Returns the number of slots reclaimed. Counter feeds the
// orphan_reclaimed telemetry field.
//
// SAFETY: only callable from request_end's compaction phase, NEVER
// from inside a query body. The engine_compact assert enforces this.
// ----------------------------------------------------------------------------

uint64_t db_engine_reclaim_orphans(db_query_ctx *ctx, uint64_t threshold_rev);

// ----------------------------------------------------------------------------
// Dispatch table
//
// recompute_dispatch[kind] is the wrapper-call thunk for each QueryKind.
// db_verify uses this to pull a recorded dep: it calls the wrapper,
// which handles its own cache-vs-recompute via DB_QUERY_GUARD; after
// the call, the dep slot's fingerprint reflects its current value.
//
// Populated at db_init via db_engine_register_dispatch (NOT a per-file
// thing — the X-macro in engine_dispatch.c iterates ORE_QUERY_KINDS and
// produces a static initializer).
//
// Exhaustiveness is compile-time enforced: a new kind without a thunk
// is a missing-symbol link error.
// ----------------------------------------------------------------------------

typedef void (*RecomputeFn)(db_query_ctx *ctx, uint64_t key);
extern const RecomputeFn db_engine_recompute_dispatch[QUERY_KIND_COUNT];

// Initialize the engine — call once from db_init. Sets up the dispatch
// table reference, the running-slots tracker, etc.
void db_engine_init(db_query_ctx *ctx);
void db_engine_free(db_query_ctx *ctx);

// ----------------------------------------------------------------------------
// Verify
//
// Called from db_query_begin for cached slots. Walks recorded deps,
// pulls each via the dispatch table, compares the dep's current fp
// to the recorded dep_fp. Any mismatch → slot is stale → caller
// recomputes.
//
// Per-input durability: verify computes MIN over LIVE deps' tiers,
// not the slot's frozen MIN-at-succeed-time. Stale deps (verified_rev
// < current) don't influence the MIN.
// ----------------------------------------------------------------------------

bool db_engine_verify(db_query_ctx *ctx, QuerySlotHot *slot);

// ----------------------------------------------------------------------------
// Slot routing
//
// Resolves (kind, key) to a slot row in the appropriate SoA column.
// Returns the hot and cold pointers via out-params. NULL out-params on
// route failure (unwired kind, OOB key, etc.) — locate_slot/_cold
// return NULL in that case.
//
// The mapping from kind to storage location is kind-specific (some
// kinds are HashMap-routed, others are Vec-indexed). This is the only
// place that knowledge lives.
// ----------------------------------------------------------------------------

bool db_engine_route_slot(db_query_ctx *ctx, QueryKind kind, uint64_t key,
                          QuerySlotHot **out_hot, QuerySlotCold **out_cold);

// ----------------------------------------------------------------------------
// Request-scoped running-slot tracker
//
// Slots that transitioned to RUNNING are pushed onto a request-scoped
// list. db_request_end sweeps the list and resets any leftover RUNNING
// slots to EMPTY (defends against bodies that bail mid-execution —
// cancellation, panic, future cancellation-honoring guard paths).
// ----------------------------------------------------------------------------

typedef struct {
    QueryKind kind;
    uint64_t  key;
} QueryRunningRef;

void db_engine_track_running(db_query_ctx *ctx, QueryKind kind, uint64_t key);
void db_engine_sweep_running(db_query_ctx *ctx);

// ----------------------------------------------------------------------------
// Compaction (called from db_request_end)
//
// Triggers mark-and-copy compaction across the shared pools (decl_pool,
// body_scope_*) AND across slot tables (orphan reclamation). Threshold-
// gated — runs only when count growth justifies the cost.
// ----------------------------------------------------------------------------

void db_engine_compact(db_query_ctx *ctx);

// ----------------------------------------------------------------------------
// Slot pointer access — engine-internal
//
// Layer code reads slot state through engine.h's (ctx, kind, key) accessors.
// Engine code uses these for the hot path (no double-route lookup per
// access). The pointer is invalidated by any subsequent column realloc
// (SoA Vec grow) — callers must NOT hold it across nested query calls.
// ----------------------------------------------------------------------------

QuerySlotHot  *db_engine_locate_slot(db_query_ctx *ctx, QueryKind kind, uint64_t key);
QuerySlotCold *db_engine_locate_slot_cold(db_query_ctx *ctx, QueryKind kind, uint64_t key);

// db_diags_clear lives in src/db/diag/diag.h; engine.c includes that
// header directly. Do not re-declare here.

#endif // ORE_DB_QUERY_ENGINE_INTERNAL_H
