#ifndef SEMA_EVAL_CONST_EVAL_H
#define SEMA_EVAL_CONST_EVAL_H

#include <stdint.h>
#include <stdbool.h>

#include "../../db/query/query.h"
#include "../../db/intern_pool/intern_pool.h"

struct Sema;
struct Expr;

typedef enum {
    CONST_NONE,
    CONST_INT,
    CONST_FLOAT,
    CONST_BOOL,
} ConstKind;

struct ConstValue {
    ConstKind kind;
    union {
        int64_t int_val;
        double float_val;
        bool bool_val;
    };
};

// Per-Expr const-eval cache entry. Lives on Sema.const_eval_entries
// keyed by NodeId. Owns its query slot so cycle detection +
// invalidation work the same way they do for every other query.
struct ConstEvalEntry {
    struct ConstValue value;
    struct QuerySlot query;
};

// Per-Expr "is this comptime-evaluable?" cache entry. Mirrors
// ConstEvalEntry's shape — separate hashmap on Sema so the predicate
// invalidates independently of the value-folding query (different
// invalidation cadence: a query_const_eval miss for a yet-unsupported
// Expr kind shouldn't shift is_comptime's answer).
struct IsComptimeEntry {
    bool result;
    struct QuerySlot query;
};

struct ConstValue query_const_eval(struct Sema *s, struct Expr *expr);
bool             query_is_comptime(struct Sema *s, struct Expr *expr);

// === R4 Step 4: ConstValue ↔ IpIndex bridge ===
//
// Unlike `struct Type *` which is pointer-shared and needs an in-band
// IpIndex field, `struct ConstValue` is passed by value — identity
// isn't a concern at the value level. Pool integration here is one-
// way-on-demand:
//
//   const_value_to_ip(s, v) — intern v and return its IpIndex.
//     Idempotent: two equal ConstValues map to the same IpIndex.
//     Bool true/false map to the reserved IP_BOOL_TRUE/IP_BOOL_FALSE.
//     Returns IP_NONE for CONST_NONE input.
//
//   const_value_from_ip(s, idx) — reverse lookup. Reads the pool's
//     tag + extra slots and reconstructs the ConstValue. Returns a
//     CONST_NONE struct on IP_NONE / unrecognized / non-value idx.
//
// Step 5+ (introspection builtins like @TypeOf on a comptime value)
// is where these get called from production paths. Today: bridge
// available, no required consumer.
IpIndex            const_value_to_ip(struct Sema *s, struct ConstValue v);
struct ConstValue  const_value_from_ip(struct Sema *s, IpIndex idx);

#endif