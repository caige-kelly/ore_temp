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

    // Layer 4 — scopes & resolution
    QUERY_SCOPE_FOR_NODE,
    QUERY_SCOPE_DECLS,
    QUERY_SCOPE_PARENT,
    QUERY_EFFECT_OPS_VISIBLE,
    QUERY_RESOLVE_REF,
    QUERY_RESOLVE_PATH,

    // Stubbed — declared so the engine knows the kind enum, real impl deferred.
    QUERY_CONST_EVAL,
} QueryKind;

typedef enum {
    QUERY_BEGIN_COMPUTE,
    QUERY_BEGIN_CACHED,
    QUERY_BEGIN_CYCLE,
    QUERY_BEGIN_ERROR,
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
// query's compute. Recorded on the *parent* frame as the child query
// runs; stamped onto the parent slot's `deps` Vec when the parent
// succeeds. Today nothing consumes the recorded deps; the recording is
// the hook that makes incremental possible later.
struct QueryDep {
    QueryKind kind;
    const void* key;     // borrowed; key lifetime must outlive the slot
};

struct QuerySlot {
    QueryState state;
    QueryKind kind;

    // Hooked for future incremental — see Fingerprint comment above.
    Fingerprint fingerprint;
    uint64_t computed_rev;
    uint64_t verified_rev;

    // Vec<QueryDep> recorded during the last successful compute. NULL
    // when the slot has never computed or computed with zero deps.
    // Lifetime: lives in sema->arena alongside the slot's owner.
    Vec* deps;
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
};

void sema_query_slot_init(struct QuerySlot* slot, QueryKind kind);
QueryBeginResult sema_query_begin(struct Sema* sema, struct QuerySlot* slot,
    QueryKind kind, const void* key, struct Span span);
void sema_query_succeed(struct Sema* sema, struct QuerySlot* slot);
void sema_query_fail(struct Sema* sema, struct QuerySlot* slot);
const char* sema_query_kind_str(QueryKind kind);

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
