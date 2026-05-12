#ifndef SEMA_QUERY_H
#define SEMA_QUERY_H

#include <stdbool.h>
#include <stdint.h>

#include "../../common/vec.h"
#include "../../lexer/token.h"

struct Sema;

typedef enum {
    QUERY_EMPTY,
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

    // Layer 3 — modules
    QUERY_MODULE_AST,
    QUERY_MODULE_DEF_MAP,
    QUERY_MODULE_EXPORTS,
    QUERY_MODULE_FOR_PATH,
    QUERY_TOP_LEVEL_INDEX,
    QUERY_DEF_FOR_NAME,

    // Layer 4 — scopes & resolution
    QUERY_SCOPE_FOR_NODE,
    QUERY_SCOPE_DECLS,
    QUERY_SCOPE_PARENT,
    QUERY_EFFECT_OPS_VISIBLE,
    QUERY_RESOLVE_REF,
    QUERY_RESOLVE_PATH,
    QUERY_NODE_TO_DECL,
    QUERY_FN_SCOPE_INDEX,

    // Stubbed — declared so the engine knows the kind enum, real impl deferred.
    QUERY_CONST_EVAL,

    // Layer E.2 — per-Expression type computation.
    QUERY_TYPE_OF_EXPR,

    // Layer E.2 — per-fn signature (param types, ret, modifiers).
    // Split from QUERY_TYPE_OF_DECL so body Idents that resolve to a
    // param can read their own fn's signature without re-entering the
    // outer fn-type query (which is RUNNING during body checking).
    QUERY_FN_SIGNATURE,

    // Layer E.3 — per-struct signature (resolved field types, with
    // C-style anonymous union arms flattened into the same arena).
    // Field DefIds resolve their type by indexing into this signature
    // via FieldLocator. Separate slot is required for the same reason
    // as QUERY_FN_SIGNATURE: identity-only TY_STRUCT can be produced
    // before fields are resolved, but field-type resolution may
    // recurse into other struct types (consider `^Self` shapes).
    QUERY_STRUCT_SIGNATURE,

    // Layer E.3 — per-enum signature (variant name + explicit/auto-
    // incremented value). Variant DefIds resolve their type via
    // VariantLocator (each variant's "type" is the parent enum).
    QUERY_ENUM_SIGNATURE,

    // Layer E.3.5 — per-Expr "is this comptime-evaluable?" predicate.
    // Replaces a recursive walker that bypassed the dep graph; with a
    // real query, editing a transitively-referenced const-bind
    // invalidates every dependent comptime-check via fingerprint
    // mismatch. Slot is keyed by IsComptimeEntry; lives in
    // s->is_comptime_entries (per-NodeId).
    QUERY_IS_COMPTIME,
} QueryKind;

// Number of QueryKind variants. Used to size the per-kind telemetry
// table under ORE_DEBUG_QUERIES. Update alongside the enum — adding
// a new variant without bumping this would silently truncate stats.
// Kept as a #define rather than a sentinel enum variant so existing
// exhaustive switches (sema_query_kind_str, sema_locate_slot) stay
// valid without a no-op `case` line.
#define QUERY_KIND_COUNT ((int)QUERY_IS_COMPTIME + 1)

typedef enum {
    QUERY_BEGIN_COMPUTE,
    QUERY_BEGIN_CACHED,
    QUERY_BEGIN_CYCLE,
    QUERY_BEGIN_ERROR,
    QUERY_BEGIN_CANCELED,    // Layer 7.7 — request was cancelled
                             // before this query could proceed
} QueryBeginResult;

// Fingerprint of a query result. Computed by the owning query when it
// finalizes. Today nothing reads this — the field is reserved so adding
// incremental invalidation later is mechanical (compare slot.fingerprint
// to a freshly-computed hash; if equal, downstream queries skip rerun).
//
// FINGERPRINT_NONE means "no result computed yet" or "this query opted
// out of fingerprinting." Distinguishable from a real zero-hash because
// real fingerprints sprinkle high bits.
typedef uint64_t Fingerprint;
#define FINGERPRINT_NONE ((Fingerprint)0)

// One edge in the dependency graph: a query that ran during another
// query's compute. Recorded on the *parent* frame at child-succeed
// time so `dep_fp` captures the child's final fingerprint; stamped
// onto the parent slot's `deps` Vec when the parent itself succeeds.
//
// Layer 7.5 — invalidation walker — reads `dep_fp` to do early
// cutoff: when re-validating, if the dep's *current* slot fingerprint
// matches `dep_fp`, the dep hasn't changed and the parent's cached
// value is still valid.
struct QueryDep {
    QueryKind kind;
    const void* key;          // borrowed; key lifetime must outlive the slot
    Fingerprint dep_fp;       // dep's fingerprint at the time we read it
};

struct QuerySlot {
    QueryState state;
    QueryKind kind;

    // Hooked for future incremental — see Fingerprint comment above.
    Fingerprint fingerprint;
    uint64_t computed_rev;
    uint64_t verified_rev;

    // R1 — Salsa-style backdating for introspection.
    //
    // changed_rev tracks "the revision at which this slot's value last
    // ACTUALLY changed" — distinct from computed_rev ("last successful
    // compute") and verified_rev ("last revalidation"). When a body
    // reruns and produces the same fingerprint as before, changed_rev
    // is NOT bumped (the value didn't actually change). Used today for
    // diagnostic dump output (--dump-query-stats counters track
    // stable-vs-changed computes); reserved for future use in
    // cross-session query-cache serialization if/when that becomes
    // relevant.
    //
    // last_fingerprint preserves the prior committed fingerprint
    // across a RECOMPUTE → succeed cycle so we can compare in
    // sema_query_succeed. Reset to FINGERPRINT_NONE on slot init;
    // updated in sema_query_begin's RECOMPUTE branch before the
    // current fingerprint is cleared.
    uint64_t changed_rev;
    Fingerprint last_fingerprint;

    // Layer 7.7 — LRU bookkeeping. Updated on every sema_query_begin
    // hit. Reserved for a future eviction walker — no consumer reads
    // this today, but the touch hook is cheap and keeps the option
    // open when long-running LSP sessions start needing bounded
    // memory.
    uint64_t last_accessed_rev;

    // Vec<QueryDep> recorded during the last successful compute. NULL
    // when the slot has never computed or computed with zero deps.
    // Lifetime: lives in sema->arena alongside the slot's owner.
    Vec* deps;

    // Per-slot diagnostic accumulator. Lazy: NULL until the first
    // diag_emit during this slot's compute. On RECOMPUTE, arena_reset
    // wipes the prior run's diagnostics before the body reruns; on
    // REVALIDATE_SKIP_RECOMPUTE, this Vec survives untouched so the
    // LSP collector still sees the prior diagnostics on the next pass.
    // This is the fix for bug_of_bugs catalog #7 / R2: diagnostics
    // were previously an untracked write into Sema.diags that didn't
    // re-fire on cached query recomputes.
    //
    // TODO(eviction): when the eviction walker lands, it must call
    // arena_release(diag_arena) before evicting the slot to free the
    // backing memory. Until then, the arena lives as long as the slot
    // does (typically the Sema lifetime).
    Arena* diag_arena;       // owns the backing memory for `diags`
    Vec* diags;              // Vec<struct Diag>; lives in diag_arena
    size_t diag_error_count; // mirrors DiagBag.error_count for the rollup

#ifdef ORE_DEBUG_QUERIES
    // Salsa's "DerivedUntracked" memo state. Set when the producing
    // compute body called SEMA_READ_UNTRACKED — i.e. read non-query
    // state that the dep graph can't track. The revalidation walker
    // forces RECOMPUTE on these slots regardless of dep fingerprints,
    // mirroring Salsa's behavior at salsa/derived/slot.rs (the
    // memo-with-no-verifiable-inputs case must always re-execute).
    // Propagated from QueryFrame.has_untracked_read at succeed/fail
    // time. See bug_of_bugs.md #16, R2.
    bool has_untracked_read;
#endif
};

struct QueryFrame {
    QueryKind kind;
    const void* key;
    struct Span span;

    // Accumulating deps recorded during this compute. Flushed to the
    // slot in sema_query_succeed. Allocated lazily on first dep.
    Vec* deps;

    // Backreference so sema_query_succeed can find the slot to flush
    // deps onto. Set by sema_query_begin.
    struct QuerySlot* slot;

#ifdef ORE_DEBUG_QUERIES
    // True once any SEMA_READ_UNTRACKED call fires inside this query's
    // body. Flushed onto the slot at succeed/fail time so revalidation
    // can act on it. The counter is for telemetry — how often a query
    // body reaches outside the dep graph; ideally trends to zero on
    // hot paths after migration.
    bool has_untracked_read;
    uint64_t untracked_read_count;
#endif
};

void sema_query_slot_init(struct QuerySlot* slot, QueryKind kind);
QueryBeginResult sema_query_begin(struct Sema* sema, struct QuerySlot* slot,
    QueryKind kind, const void* key, struct Span span);
void sema_query_succeed(struct Sema* sema, struct QuerySlot* slot);
void sema_query_fail(struct Sema* sema, struct QuerySlot* slot);
const char* sema_query_kind_str(QueryKind kind);

// Returns the top QueryFrame on sema->query_stack, or NULL if empty.
// Exposed so diag_emit can route a diagnostic into the currently
// executing query's slot. Caller must not mutate the returned frame's
// `slot` or `kind`/`key` fields.
struct QueryFrame* query_stack_top(struct Sema* sema);

#ifdef ORE_DEBUG_QUERIES
// Mark the active query frame as having read non-query state. Called
// only from the SEMA_READ_UNTRACKED macro (see ast_dep.h). The `why`
// string is borrowed for the lifetime of the call — pass a literal.
// No-op when the stack is empty (top-level driver code can read freely).
void sema_mark_frame_untracked(struct Sema* sema, const char* why);

// Per-QueryKind telemetry counters. Lives at sema->query_stats[kind].
// Bumped by sema_query_begin / succeed / fail and by sema_revalidate.
// Dumped via --dump-query-stats. See bug_of_bugs.md B14.
struct QueryStats {
    uint64_t begin;                       // sema_query_begin entries
    uint64_t cached_hit;                  // returned QUERY_BEGIN_CACHED
    uint64_t compute;                     // body ran to sema_query_succeed
    uint64_t cycle;                       // returned QUERY_BEGIN_CYCLE
    uint64_t error;                       // body called sema_query_fail
    uint64_t recompute_due_to_untracked;  // revalidate forced RECOMPUTE
                                          // because slot.has_untracked_read
    // R1 — of the `compute` events, how many actually shifted the
    // slot's fingerprint vs reran-but-stable. high stable counts
    // suggest over-invalidation that R6 (per-decl AST fp) could
    // address.
    uint64_t compute_value_changed;       // body produced new fingerprint
    uint64_t compute_value_stable;        // body produced same fingerprint
};

#include <stdio.h>
// Dump per-kind counter table to `out`. Header line + one row per
// QueryKind. Always succeeds; safe to call when no queries have run.
void sema_dump_query_stats(struct Sema* sema, FILE* out);
#endif

// Boilerplate-cutting helper for the begin → switch idiom. Used as:
//
//   SEMA_QUERY_GUARD(s, &info->type_query, QUERY_TYPE_OF_DECL, decl, span,
//                    /*on_cached=*/info->type,
//                    /*on_cycle=*/(report_cycle(s, decl), s->error_type),
//                    /*on_error=*/s->error_type);
//   // ... compute body ...
//
// Each branch expression is evaluated only on its respective path. The
// macro returns from the enclosing function for cached/cycle/error
// cases; on COMPUTE, control falls through.
#define SEMA_QUERY_GUARD(sema, slot, kind, key, span,                       \
                         on_cached, on_cycle, on_error)                     \
    do {                                                                    \
        QueryBeginResult __qbr =                                            \
            sema_query_begin((sema), (slot), (kind), (key), (span));        \
        if (__qbr == QUERY_BEGIN_CACHED) return (on_cached);                \
        if (__qbr == QUERY_BEGIN_CYCLE)  return (on_cycle);                 \
        if (__qbr == QUERY_BEGIN_ERROR)  return (on_error);                 \
    } while (0)

#endif // SEMA_QUERY_H
