#ifndef ORE_DB_QUERY_EVAL_H
#define ORE_DB_QUERY_EVAL_H

#include "typed_value.h"
#include "type_layer.h" // SemaCtx

// Phase 2 — unified expression evaluator. Returns a (type, value) pair:
// the TYPE tells you the expression's category (type-valued, value-of-int,
// etc.); the VALUE is the comptime-folded result if known, IP_NONE if the
// expression is runtime-only.
//
// This replaces the case-by-case dispatch in `resolve_type_expr`, which is
// now a thin wrapper that gates eval_expr's result on
// "type == IP_TYPE_TYPE OR a TYPE_VAR hole."
//
// `type_of_expr` (in infer.c) and `const_eval.c` are NOT migrated in
// Phase 2 — they coexist as parallel evaluators today and get folded into
// eval_expr in Phase 3+. There is no glue between them; eval_expr is
// self-contained.
//
// Every dispatch arm calls `node_typed_value_push(ctx, node, type, value)`
// at the end (Phase 1 plumbing) so subsequent lookups see both halves.
TypedValue eval_expr(const SemaCtx *ctx, SyntaxNode *node);

// Phase 6 Batch 3 — set the enum-context hint on a local SemaCtx copy and
// dispatch to eval_expr. Single-shot: the hint is read by SK_ENUM_REF_EXPR
// to resolve bare `.variant` and by SK_BIN_EXPR's EQ_EQ/BANG_EQ retry; every
// other arm that recurses on a sub-expression resets it to DEF_ID_NONE.
// Caller passes the enum's DefId (i.e. the .zir_node_id of the enum's
// IPK_ENUM_TYPE key, as a DefId).
TypedValue eval_expr_with_enum_hint(const SemaCtx *ctx, SyntaxNode *node,
                                    DefId enum_def);

// `::` const immutability — reject reassignment / `++` / `--` of an immutable
// `::` binding (a const local or a top-level KIND_CONSTANT). Returns true (and
// emits a diag) when `target` is a bare reference to such a binding; place
// targets (field / index / deref) and mutable `:=` bindings / parameters return
// false. `verb` folds into the message ("assign to" / "modify").
bool reject_const_mutation(const SemaCtx *ctx, SyntaxNode *target,
                           const char *verb);

#endif // ORE_DB_QUERY_EVAL_H
