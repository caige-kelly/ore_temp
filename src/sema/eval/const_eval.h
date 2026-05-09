#ifndef SEMA_EVAL_CONST_EVAL_H
#define SEMA_EVAL_CONST_EVAL_H

#include <stdint.h>
#include <stdbool.h>

#include "../query/query.h"

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

#endif