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

#endif // ORE_DB_QUERY_EVAL_H
