#include "bin_ops/bin_ops.h" 
#include "literals/literals.h"
#include <stdint.h>
#include <stdlib.h>

struct ConstValue query_const_eval(struct Sema *s, struct Expr *expr) {
  struct ConstValue none = {.kind = CONST_NONE};
  if (!s || !expr)
    return none;

  // Cache check: have we computed this already?
  void *cached = hashmap_get(&s->const_eval_cache, (uint64_t)(uintptr_t)expr);
  if (cached) {
    return *(struct ConstValue *)cached;
  }

  // Compute.
  struct ConstValue result = none;
  switch (expr->kind) {
  case expr_Lit: {
    if (expr->lit.kind == lit_Int) {
      const char *text = pool_get(s->pool, expr->lit.string_id, 0);
      int64_t v;
      if (parse_int_literal(text, &v)) {
        result.kind = CONST_INT;
        result.int_val = v;
      }
    }
    if (expr->lit.kind == lit_Float) {
      const char *text = pool_get(s->pool, expr->lit.string_id, 0);
      double v;
      if (parse_float_literal(text, &v)) {
        result.kind = CONST_FLOAT;
        result.float_val = v;
      }
    }
    break;
  }
  case expr_Bin: {
    struct ConstValue l = query_const_eval(s, expr->bin.Left);
    struct ConstValue r = query_const_eval(s, expr->bin.Right);

    if (l.kind == CONST_NONE || r.kind == CONST_NONE) break;

    switch (expr->bin.op) {
      case Plus:  result = bin_add(s, expr, l, r); break;
      default: break;
    }
    break;
  }
  default:
    break;
  }

  // Cache the result. Allocate in the arena so it lives as long as Sema.
  struct ConstValue *stored = arena_alloc(s->arena, sizeof(*stored));
  *stored = result;
  hashmap_put(&s->const_eval_cache, (uint64_t)(uintptr_t)expr, stored);

  return result;
}