#include "const_eval.h"

#include <stdint.h>
#include <stdlib.h>

#include "../../common/arena.h"
#include "../../common/hashmap.h"
#include "../../common/stringpool.h"
#include "../../diag/diag.h"
#include "../../parser/ast.h"
#include "../query/query_engine.h"
#include "../sema.h"
#include "bin_ops/bin_ops.h"
#include "literals/literals.h"

// === Entry lookup / lazy creation ===
//
// One ConstEvalEntry per Expr we ever try to const-eval, keyed by
// NodeId so the table survives re-parses cleanly (NodeIds are unique
// per parse; raw Expr* pointers aren't). The entry holds the slot so
// SEMA_QUERY_GUARD can do cycle detection and (eventually) early
// cutoff via fingerprints.
static struct ConstEvalEntry *const_eval_entry_for(struct Sema *s,
                                                   struct Expr *expr) {
  if (s->const_eval_entries.entries == NULL)
    hashmap_init_in(&s->const_eval_entries, &s->arena);

  uint64_t key = (uint64_t)expr->id.id;
  if (hashmap_contains(&s->const_eval_entries, key))
    return (struct ConstEvalEntry *)hashmap_get(&s->const_eval_entries, key);

  struct ConstEvalEntry *e = arena_alloc(&s->arena, sizeof(*e));
  *e = (struct ConstEvalEntry){0};
  sema_query_slot_init(&e->query, QUERY_CONST_EVAL);
  hashmap_put_or_die(&s->const_eval_entries, key, e, "const_eval_entries");
  return e;
}

// === Compute helpers ===

static struct ConstValue eval_lit(struct Sema *s, struct Expr *expr) {
  struct ConstValue r = {.kind = CONST_NONE};
  const char *text = pool_get(&s->pool, expr->lit.string_id, 0);
  if (!text)
    return r;
  if (expr->lit.kind == lit_Int) {
    int64_t v;
    if (parse_int_literal(text, &v)) {
      r.kind = CONST_INT;
      r.int_val = v;
    }
  } else if (expr->lit.kind == lit_Float) {
    double v;
    if (parse_float_literal(text, &v)) {
      r.kind = CONST_FLOAT;
      r.float_val = v;
    }
  }
  return r;
}

static struct ConstValue eval_bin(struct Sema *s, struct Expr *expr) {
  struct ConstValue l = query_const_eval(s, expr->bin.Left);
  struct ConstValue r = query_const_eval(s, expr->bin.Right);
  if (l.kind == CONST_NONE || r.kind == CONST_NONE)
    return (struct ConstValue){.kind = CONST_NONE};

  switch (expr->bin.op) {
  case Plus: return bin_add(s, expr, l, r);
  default:   return (struct ConstValue){.kind = CONST_NONE};
  }
}

static struct ConstValue eval_unary(struct Sema *s, struct Expr *expr) {
  // Only prefix arithmetic-shaped operators are constant-evaluable.
  // Postfix `^` (deref), `?` (de-nil), `++`, `--` and address-of `&`
  // are runtime-shaped; they don't apply to comptime-known scalars.
  if (expr->unary.postfix)
    return (struct ConstValue){.kind = CONST_NONE};

  struct ConstValue v = query_const_eval(s, expr->unary.operand);
  if (v.kind == CONST_NONE)
    return v;

  switch (expr->unary.op) {
  case unary_Neg:
    if (v.kind == CONST_INT) {
      // Detect INT64_MIN negation (UB on signed overflow).
      if (v.int_val == INT64_MIN) {
        diag_error(&s->diags, expr->span,
                   "int overflow during unary negation");
        return (struct ConstValue){.kind = CONST_NONE};
      }
      return (struct ConstValue){.kind = CONST_INT, .int_val = -v.int_val};
    }
    if (v.kind == CONST_FLOAT) {
      return (struct ConstValue){.kind = CONST_FLOAT, .float_val = -v.float_val};
    }
    return (struct ConstValue){.kind = CONST_NONE};
  case unary_BitNot:
    if (v.kind == CONST_INT)
      return (struct ConstValue){.kind = CONST_INT, .int_val = ~v.int_val};
    return (struct ConstValue){.kind = CONST_NONE};
  case unary_Not:
    // Bool not is comptime-known if the operand is. We don't yet
    // have a CONST_BOOL kind; defer until bool literals flow through
    // the const lattice.
    return (struct ConstValue){.kind = CONST_NONE};
  default:
    // Type-shaped unaries (Const, Optional, Ptr, ManyPtr) and
    // address/deref/inc/dec aren't const-evaluable in the int/float
    // lattice. Stay non-constant.
    return (struct ConstValue){.kind = CONST_NONE};
  }
}

// === Query body ===

struct ConstValue query_const_eval(struct Sema *s, struct Expr *expr) {
  struct ConstValue none = {.kind = CONST_NONE};
  if (!s || !expr || expr->id.id == 0)
    return none;

  struct ConstEvalEntry *entry = const_eval_entry_for(s, expr);

  SEMA_QUERY_GUARD(s, &entry->query, QUERY_CONST_EVAL, entry, expr->span,
                   /*on_cached=*/entry->value,
                   /*on_cycle=*/none,
                   /*on_error=*/none);

  struct ConstValue result = none;
  switch (expr->kind) {
  case expr_Lit:   result = eval_lit(s, expr); break;
  case expr_Bin:   result = eval_bin(s, expr); break;
  case expr_Unary: result = eval_unary(s, expr); break;
  // expr_Ident lands when name resolution + decl const lookup are
  // wired together — depends on `query_const_eval_for_def` (Stage A
  // follow-up), not just resolve.
  default: break;
  }

  entry->value = result;

  // Fingerprint the result so the future invalidator can do early
  // cutoff. CONST_NONE stays at FINGERPRINT_NONE so misses aren't
  // confused with "valid zero" downstream.
  if (result.kind != CONST_NONE) {
    Fingerprint fp = query_fingerprint_from_bytes(&result, sizeof(result));
    query_slot_set_fingerprint(&entry->query, fp);
  }

  sema_query_succeed(s, &entry->query);
  return result;
}
