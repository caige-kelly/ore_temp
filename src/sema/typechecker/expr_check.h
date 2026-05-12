#ifndef ORE_SEMA_TYPE_EXPR_CHECK_H
#define ORE_SEMA_TYPE_EXPR_CHECK_H

#include <stdbool.h>

#include "../query/query.h"

struct Sema;
struct Expr;
struct Type;

// Per-Expression type-cache entry. Keyed in `Sema.type_of_expr_entries`
// by NodeId (one slot per AST expression). Standard SEMA_QUERY_GUARD
// machinery — cycle detection, dep recording, fingerprint.
struct TypeOfExprEntry {
    struct Type *type;
    struct QuerySlot query;
};

// Synthesize the type of `expr` (bidirectional "synth" half).
//
// Walks the expression tree, recursively typing children via further
// query_type_of_expr calls (which records the deps automatically).
// Reads name resolutions via query_resolve_ref → query_type_of_def
// for Idents. Builds compound types via the interners in type/intern.c.
//
// Returns the expression's natural type. For a `comptime_int` literal
// or arithmetic over them, returns `comptime_int` — *not* the
// contextually-coerced concrete type. Coercion happens at use sites
// via `check_expr` / `coerce` (the bidirectional "check" half).
//
// Always returns non-NULL; on error returns `s->error_type` and emits
// a diagnostic (deduped by span).
struct Type *query_type_of_expr(struct Sema *s, struct Expr *expr);

// Bidirectional "check" half: ensure `expr`'s synthesized type
// coerces to `expected`. Calls query_type_of_expr to get the natural
// type, then coerce() to validate. Emits a diagnostic on mismatch.
//
// `expected` may be NULL — in that case behaves as a synthesis-only
// call (returns true if the expr typed without error).
bool check_expr(struct Sema *s, struct Expr *expr, struct Type *expected);

#endif
