#ifndef ORE_DB_QUERY_H
#define ORE_DB_QUERY_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "../../support/data_structure/vec.h"
#include "../../support/data_structure/arena.h"
#include "../ids/ids.h"

// =============================================================================
// QUERY ENGINE INVARIANTS
//
// Reference for what the engine guarantees, what it does NOT guarantee,
// and what callers must respect. Memorize before adding a new query
// kind or touching the engine.
//
// 1. SLOT STATES
//
//   QuerySlotHot.state is one of:
//     QUERY_EMPTY   — never computed for this revision-chain, OR was
//                     reset to EMPTY by input invalidation / the
//                     db_request_end RUNNING sweep.
//     QUERY_RUNNING — currently being computed (or revalidated). Set
//                     by db_query_begin on the COMPUTE / verify paths.
//                     TRANSIENT — must not survive a request boundary.
//     QUERY_DONE    — body called db_query_succeed; fingerprint + deps
//                     recorded; result cached.
//     QUERY_ERROR   — body called db_query_fail; cached as a failure
//                     result (downstream sees QUERY_BEGIN_ERROR).
//
//   Transitions:
//     EMPTY → RUNNING (db_query_begin COMPUTE) → DONE/ERROR (succeed/fail)
//     DONE/ERROR → RUNNING (verify failed → recompute) → DONE/ERROR
//     DONE/ERROR → EMPTY (input setter invalidation)
//     RUNNING → EMPTY (db_request_end sweep if no clean transition)
//
// 2. REVISION SEMANTICS
//
//   current_revision   : global, monotonic. Bumped by input setters
//                        via db_input_changed(dur). Lives in
//                        REV_CURRENT_MASK bits of rev_control.
//   effective_revision : pinned to current_revision at db_request_begin
//                        via REV_REQUEST_MASK. Stays fixed for the
//                        request — queries see a consistent snapshot.
//   dur_last_changed[d]: per-durability "last input of this tier
//                        changed at" revision. Drives the verify
//                        fast-path: a slot at durability D skips its
//                        dep walk if dur_last_changed[D] <= its
//                        verified_rev.
//
//   Inputs may be mutated BETWEEN requests; setters bump
//   current_revision. Inputs may NOT be mutated INSIDE a query body
//   — the purity invariant. Lazy disk reads via
//   workspace_resolve_import are pure (they admit new sources via
//   no-bump setters; no input mutation).
//
// 3. REQUEST BOUNDARIES
//
//   db_request_begin(rev):
//     - asserts no query is on the stack
//     - sets effective_revision = rev (typically current_revision)
//     - resets request_arena
//     - clears running_slots tracking list
//
//   Between begin and end:
//     - effective_revision is pinned
//     - bodies may freely call other queries (memoization handles
//       caching), record deps, emit diagnostics
//     - input setters MUST NOT be called from inside a query body
//     - request_arena is the only legal "live across calls within
//       this request" scratch storage
//
//   db_request_end:
//     - sweeps running_slots: any slot still in RUNNING (body bailed
//       out without succeed/fail, e.g. cancellation) is reset to
//       EMPTY (Phase 1f safety net)
//     - unpins effective_revision
//     - resets request_arena
//     - asserts no query is on the stack
//
// 4. CACHEABILITY RULES
//
//   DONE slots cache until invalidated; cache hit returns the slot's
//   fingerprint without re-running the body.
//
//   ERROR slots cache too — repeated calls return QUERY_BEGIN_ERROR
//   without re-running. Same invalidation paths as DONE.
//
//   RUNNING slots are never cache-hit by their own re-entry; instead,
//   db_query_begin returns QUERY_BEGIN_CYCLE. The cycling caller's
//   DB_QUERY_GUARD returns the cycle sentinel; the body does NOT
//   call succeed/fail; the slot's transient RUNNING is reaped at
//   request_end.
//
//   CYCLE results are NOT cached. The slot stays RUNNING during the
//   cycling call's lifetime, then resets to EMPTY at request_end.
//   Next request retries from scratch.
//
// 5. INVALIDATION GUARANTEES
//
//   Slot.durability = MIN over (deps' durabilities, noted untracked-
//   input durabilities). Computed at db_query_succeed.
//
//   Verify fast-path:
//     if dur_last_changed[slot.durability] <= slot.verified_rev:
//       slot's value provably unchanged → skip dep walk
//
//   Slow path: walk recorded deps; for each, pull the dep's current
//   fingerprint via the dispatch table. Any dep_fp mismatch → slot
//   is stale → recompute.
//
//   has_untracked_read = true bypasses the fast-path; always re-runs.
//
//   Input invalidation paths:
//     - db_set_source_text: bumps DUR_LOW, stales QUERY_FILE_AST slots
//     - db_create_file: bumps DUR_MEDIUM (gated on owner's
//       top_level_index already being DONE/ERROR)
//     - workspace_did_change_external: same as did_change
//     - workspace_did_evict_source: sets evicted bit, bumps DUR_MEDIUM
//     - workspace_admit_virtual: NO bump (admits a previously-
//       unobserved file; Roslyn "lazy inputs" model)
//
// 6. DIAGNOSTIC SURVIVAL
//
//   Diags live in a centralized table (db.diags) keyed by
//   (QueryKind, key) — NOT per-slot. They are NOT a salsa value.
//
//   Lifecycle:
//     - Emitted via db_emit_* from inside a query body; routed to
//       the active frame's analysis unit.
//     - Cleared at db_query_begin's recompute path (slot
//       transitioned to RUNNING for re-execution; old diags dropped).
//     - Cleared by input setters that stale a slot
//       (db_set_source_text, db_create_file, workspace_did_evict_source).
//     - Cached when a slot reaches DONE: subsequent cached-hit calls
//       "replay" the diags via db_collect_diags_for_file.
//
//   Diags emitted in a CYCLE path attach to the active (cycling)
//   frame's analysis unit. That frame's slot is RUNNING and gets
//   reset at request_end → its diags are dropped with it.
//
// 7. PARTIAL-FAILURE / CANCELLATION
//
//   db_query_fail is a clean failure: ERROR state, fingerprint
//   recorded, deps adopted, frame popped. Equivalent to succeed but
//   caches a failure.
//
//   Cancellation (db_check_cancel → true) returns QUERY_BEGIN_CANCELED
//   at the TOP of db_query_begin, before any slot mutation. The
//   DB_QUERY_GUARD macro does NOT handle CANCELED today — for now
//   cancellation relies on the Phase 1f RUNNING sweep at request_end
//   to reap any leftover state from a mid-bodied bail.
//
// 8. INVARIANTS BODIES MUST RESPECT
//
//   - Every body that successfully computes a result MUST call exactly
//     one of db_query_succeed or db_query_fail before returning.
//   - Bodies must NOT call input setters or db_input_changed.
//   - Bodies must NOT hold slot pointers across nested db_query_*
//     calls — column reallocs invalidate them. Re-locate via
//     db_locate_slot.
//   - Bodies must use the DB_QUERY_GUARD macro for cycle handling.
//     Manual cycle return paths are an anti-pattern.
//   - Bodies may allocate freely in request_arena (per-request
//     scratch) but NOT in slot->diag_arena (owned by the diag system).
// =============================================================================

struct db;

typedef enum {
    QUERY_EMPTY = 0,
    QUERY_RUNNING,
    QUERY_DONE,
    QUERY_ERROR,
} QueryState;

typedef enum {
    QUERY_TYPE_OF_DECL,
    QUERY_LAYOUT_OF_TYPE,
    QUERY_INSTANTIATE_DECL,
    QUERY_EFFECT_SIG,
    QUERY_BODY_EFFECTS,
    QUERY_MODULE_AST,
    QUERY_FILE_AST,
    QUERY_NAMESPACE_SCOPES,
    QUERY_TOP_LEVEL_INDEX,
    QUERY_SCOPE_FOR_NODE,
    QUERY_SCOPE_DECLS,
    QUERY_SCOPE_PARENT,
    QUERY_EFFECT_OPS_VISIBLE,
    QUERY_RESOLVE_REF,
    QUERY_RESOLVE_PATH,
    QUERY_DEF_IDENTITY,
    QUERY_NODE_TO_DECL,
    QUERY_FN_SCOPE_INDEX,
    QUERY_CONST_EVAL,
    QUERY_INFER_BODY,
    QUERY_FN_SIGNATURE,
    QUERY_STRUCT_SIGNATURE,
    QUERY_ENUM_SIGNATURE,
    QUERY_IS_COMPTIME,
    QUERY_BODY_STORE,
    QUERY_BODY_SCOPES,
    // Per-decl AST handle — keyed by packed (file_local<<32 | ast_id).
    // Fingerprint is a position-independent structural hash of one
    // top-level decl's AST subtree; sema queries depend on it (not on
    // the whole-file QUERY_FILE_AST) so a sibling edit early-cuts them.
    QUERY_DECL_AST,

    // Per-file @import refs — walks the file's AST and collects every
    // AST_EXPR_BUILTIN(name="import", path) into FileArray<ImportRef>.
    // Pure: depends on QUERY_FILE_AST. Workspace's discovery loop uses
    // this to know what to load next.
    QUERY_FILE_IMPORTS,

    // Per-namespace struct type. Returns the IpIndex of the namespace's
    // anonymous struct type (IPK_NAMESPACE_TYPE) — fields are the file's
    // public top-level decls, with lazy per-decl type resolution. See
    // src/db/query/namespace_type.h.
    QUERY_NAMESPACE_TYPE,
} QueryKind;

#define QUERY_KIND_COUNT ((int)QUERY_NAMESPACE_TYPE + 1)

typedef enum {
    QUERY_BEGIN_COMPUTE,
    QUERY_BEGIN_CACHED,
    QUERY_BEGIN_CYCLE,
    QUERY_BEGIN_ERROR,
    QUERY_BEGIN_CANCELED,
} QueryBeginResult;

typedef uint64_t Fingerprint;
#define FINGERPRINT_NONE ((Fingerprint)0)

// Typed wrapper dispatch. db.recompute_dispatch[kind] is the wrapper-call
// thunk for each active QueryKind: decodes the u64 key into the wrapper's
// typed args (FileId, DefId, packed (nsid,ast_id), etc.) and calls the
// wrapper. db_verify uses this to pull a recorded dep — the wrapper
// handles cache-vs-recompute internally via its own DB_QUERY_GUARD, so
// the engine never invokes a recompute directly.
//
// Populated by db_register_query_dispatch (src/db/query/dispatch.c —
// the only file that bridges engine and consumer types) at db_init time.
typedef void (*RecomputeFn)(struct db *s, uint64_t key);

typedef struct {
    uint64_t    key;     // the query key BY VALUE — see db_locate_slot
    Fingerprint dep_fp;
    QueryKind   kind;
    // Field order: the two u64s first, then the enum — so the enum's
    // tail padding packs three QueryDeps into 40 B, not 72. The dep
    // walk in db_revalidate reads all three fields per visit (AoS is
    // correct here); this is purely a packing win.
} QueryDep;

// Request-scoped tracking of slots that transitioned to QUERY_RUNNING.
// db_request_end sweeps these and resets any that didn't reach
// db_query_succeed/fail (defensive — covers cancellation paths and
// future patterns where a body might exit without explicit succeed).
// Stored as (kind, key) rather than slot pointers because nested query
// calls may realloc the per-kind slot column, invalidating pointers.
typedef struct {
    QueryKind kind;
    uint64_t  key;
} QueryRunningRef;

// A query's memoized slot state is split into two parallel SoA columns —
// hot and cold — at the same row index. db_revalidate (the hottest
// incremental path, run transitively per dep) reads only the hot fields;
// keeping the cold lifecycle bookkeeping out of its scan means each slot
// visit touches one cache line, not two.
//
// HOT — read/written by db_verify and db_query_begin every visit.
// db_locate_slot returns this. One cache line.
//
// Cycle detection rides on `state == QUERY_RUNNING` — verify pushes the
// frame and sets RUNNING before calling db_verify, so a recursive pull
// of the same slot hits the existing QUERY_RUNNING → QUERY_BEGIN_CYCLE
// path. There is no separate "in-progress verify" marker.
typedef struct QuerySlotHot {
    QueryState   state;
    QueryKind    kind;
    uint8_t      has_untracked_read;
    // Min (most-volatile) durability over this slot's inputs. Set at
    // db_query_succeed. Default DUR_LOW (conservative — disables the
    // durability fast-path) until a dep or noted input proves higher.
    uint8_t      durability;
    uint8_t      _pad0[2];
    Fingerprint  fingerprint;       // u64 — the memoized result fingerprint
    uint64_t     verified_rev;      // last revision proven current
    Vec         *deps;              // *Vec<QueryDep>, lazy-alloc
    // Diagnostics are NOT stored on the slot. They live in db.diags,
    // keyed by the (kind, key) analysis unit — see db.h DiagList.
} QuerySlotHot;

// COLD — read by tests (sema_rev in decl_incremental_test); production
// uses the hot fingerprint comparison directly. Same row index as the
// hot column; db_locate_slot_cold returns this.
typedef struct QuerySlotCold {
    uint64_t     computed_rev;
} QuerySlotCold;

typedef struct QueryFrame {
    QueryKind kind;
    uint64_t  key;
    // Min-durability accumulator for the running query. dur_set stays
    // false until a dep is recorded or db_query_note_input_durability
    // is called; db_query_succeed writes DUR_LOW if still unset (an
    // untracked/input query that didn't declare a tier — conservative).
    uint8_t min_input_dur;
    bool    dur_set;
    Vec* deps;   // arena-allocated Vec object (stable across query_stack realloc);
                 // backing buffer is malloc-owned by the Vec. Initialized from
                 // slot->deps so a recompute reuses the prior buffer.
#ifdef ORE_DEBUG_QUERIES
    bool has_untracked_read;
    uint64_t untracked_read_count;
#endif
} QueryFrame;

#ifdef ORE_DEBUG_QUERIES
typedef struct QueryStats {
    uint64_t begin;
    uint64_t cached_hit;
    uint64_t compute;
    uint64_t cycle;
    uint64_t error;
    uint64_t recompute_due_to_untracked;
} QueryStats;
#endif

// Lifecycle. All three functions internally resolve the slot via
// db_locate_slot(s, kind, key) — callers never hold a slot pointer across
// a body, which makes the engine safe against Vec/HashMap reallocations
// during nested query execution. Slots are zero-initialized by the
// vec_push_zero that grows their column (QUERY_EMPTY == 0, DUR_LOW == 0),
// so there is no slot-init function.
//
// db_query_succeed takes the result fingerprint as a parameter (folds in
// the previous db_query_slot_set_fingerprint step), so each phase
// boundary does exactly one db_locate_slot call.
// The query key is a BY-VALUE uint64_t. Every key fits: DefId / FileId /
// NamespaceId / StrId are u32; def_identity / resolve_ref keys are packed
// u64. db_locate_slot interprets the value per kind. By-value (vs the
// former const void*) means dep keys can't dangle and dedup is canonical.
QueryBeginResult  db_query_begin(struct db *s, QueryKind kind, uint64_t key);
void              db_query_succeed(struct db *s, QueryKind kind,
                                   uint64_t key, Fingerprint fp);
void              db_query_fail(struct db *s, QueryKind kind, uint64_t key);

// Declare that the running query read an input of the given durability
// (a Durability value; uint8_t to avoid a db.h<->query.h include
// cycle). e.g. QUERY_FILE_AST reading its source text. Lowers the
// current frame's min-durability accumulator. Idempotent.
void              db_query_note_input_durability(struct db *s, uint8_t dur);

// Bump the global revision and stamp durability tier `dur` as changed
// at it. The edit/LSP layer calls this when an input of that
// durability changes; db_revalidate then early-cuts slots whose
// durability tier is unaffected. Returns the new global revision.
uint64_t          db_input_changed(struct db *s, uint8_t dur);

QueryFrame       *db_query_stack_top(struct db *s);
const char       *db_query_kind_str(QueryKind kind);

#ifdef ORE_DEBUG_QUERIES
void  db_mark_frame_untracked(struct db *s, const char *why);
void  db_dump_query_stats(struct db *s, FILE *out);
#endif

#ifdef ORE_DEBUG_QUERIES
#define DB_READ_UNTRACKED(s, expr, why) \
    (db_mark_frame_untracked((s), (why)), (expr))
#else
#define DB_READ_UNTRACKED(s, expr, why) (expr)
#endif

#define DB_QUERY_GUARD(s, kind, key, on_cached, on_cycle, on_error) \
    do { \
        QueryBeginResult __qbr = db_query_begin((s), (kind), (key)); \
        if (__qbr == QUERY_BEGIN_CACHED) return (on_cached); \
        if (__qbr == QUERY_BEGIN_CYCLE)  return (on_cycle); \
        if (__qbr == QUERY_BEGIN_ERROR)  return (on_error); \
    } while (0)

#endif // ORE_DB_QUERY_H
