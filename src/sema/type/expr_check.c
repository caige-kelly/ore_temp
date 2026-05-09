#include "expr_check.h"

#include <stdio.h>

#include "../../common/arena.h"
#include "../../common/hashmap.h"
#include "../../common/vec.h"
#include "../../diag/diag.h"
#include "../../parser/ast.h"
#include "../eval/const_eval.h"
#include "../query/query_engine.h"
#include "../resolve/resolve.h"
#include "../sema.h"
#include "checker.h"     // query_type_of_def, resolve_type_expr
#include "coerce.h"
#include "display.h"
#include "type.h"

// === Per-Expr cache entry lookup ===

static struct TypeOfExprEntry *
type_of_expr_entry_for(struct Sema *s, struct Expr *expr) {
  if (s->type_of_expr_entries.entries == NULL)
    hashmap_init_in(&s->type_of_expr_entries, &s->arena);

  uint64_t key = (uint64_t)expr->id.id;
  if (hashmap_contains(&s->type_of_expr_entries, key))
    return (struct TypeOfExprEntry *)hashmap_get(&s->type_of_expr_entries,
                                                 key);

  struct TypeOfExprEntry *e = arena_alloc(&s->arena, sizeof(*e));
  *e = (struct TypeOfExprEntry){0};
  sema_query_slot_init(&e->query, QUERY_TYPE_OF_EXPR);
  hashmap_put_or_die(&s->type_of_expr_entries, key, e,
                     "type_of_expr_entries");
  return e;
}

// === Per-kind handlers (forward decl) ===

static struct Type *type_of_lit(struct Sema *s, struct Expr *e);
static struct Type *type_of_ident(struct Sema *s, struct Expr *e);
static struct Type *type_of_bin(struct Sema *s, struct Expr *e);
static struct Type *type_of_unary(struct Sema *s, struct Expr *e);
static struct Type *type_of_call(struct Sema *s, struct Expr *e);
static struct Type *type_of_lambda(struct Sema *s, struct Expr *e);
static struct Type *type_of_block(struct Sema *s, struct Expr *e);
static struct Type *type_of_if(struct Sema *s, struct Expr *e);
static struct Type *type_of_index(struct Sema *s, struct Expr *e);
static struct Type *type_of_array_type_expr(struct Sema *s, struct Expr *e);
static struct Type *type_of_bind(struct Sema *s, struct Expr *e);

// === Public entry points ===

struct Type *query_type_of_expr(struct Sema *s, struct Expr *expr) {
  if (!s || !expr || expr->id.id == 0)
    return s ? s->error_type : NULL;

  struct TypeOfExprEntry *entry = type_of_expr_entry_for(s, expr);

  SEMA_QUERY_GUARD(s, &entry->query, QUERY_TYPE_OF_EXPR, entry, expr->span,
                   /*on_cached=*/entry->type ? entry->type : s->error_type,
                   /*on_cycle=*/s->error_type,
                   /*on_error=*/s->error_type);

  struct Type *result = s->error_type;
  switch (expr->kind) {
  case expr_Lit:        result = type_of_lit(s, expr); break;
  case expr_Ident:      result = type_of_ident(s, expr); break;
  case expr_Bin:        result = type_of_bin(s, expr); break;
  case expr_Unary:      result = type_of_unary(s, expr); break;
  case expr_Call:       result = type_of_call(s, expr); break;
  case expr_Lambda:     result = type_of_lambda(s, expr); break;
  case expr_Block:      result = type_of_block(s, expr); break;
  case expr_If:         result = type_of_if(s, expr); break;
  case expr_Index:      result = type_of_index(s, expr); break;
  case expr_ArrayType:
  case expr_SliceType:
  case expr_ManyPtrType:
    result = type_of_array_type_expr(s, expr);
    break;
  case expr_Bind:
    result = type_of_bind(s, expr);
    break;
  default:
    // Field access (E.3 — needs struct types), Switch arms
    // (E.3+ patterns), Product literals (E.3 — struct constructors),
    // Handler / Mask / Effect — all defer.
    result = s->error_type;
    break;
  }

  entry->type = result;
  query_slot_set_fingerprint(&entry->query,
                             query_fingerprint_from_pointer(result));
  sema_query_succeed(s, &entry->query);
  return result;
}

bool check_expr(struct Sema *s, struct Expr *expr, struct Type *expected) {
  struct Type *actual = query_type_of_expr(s, expr);
  if (!expected) return actual->kind != TY_ERROR;
  // Hand the const-evaluated value to coerce so it can do the
  // range-check for comptime_int → concrete int / float coercions.
  // CONST_NONE is the right "unknown" for non-foldable RHSs.
  struct ConstValue v = query_const_eval(s, expr);
  return coerce(s, actual, expected, v, expr->span);
}

// =====================================================================
// Handlers
// =====================================================================

static struct Type *type_of_lit(struct Sema *s, struct Expr *e) {
  switch (e->lit.kind) {
  case lit_Int:    return s->comptime_int_type;
  case lit_Float:  return s->comptime_float_type;
  case lit_String: return s->string_type;
  case lit_Byte:   return s->u8_type;
  case lit_True:
  case lit_False:  return s->bool_type;
  case lit_Nil:    return s->nil_type ? s->nil_type : s->error_type;
  }
  return s->error_type;
}

static struct Type *type_of_ident(struct Sema *s, struct Expr *e) {
  // First try value namespace; if it misses, try type. Type-position
  // idents flow through `resolve_type_expr` from checker.c, so by the
  // time query_type_of_expr sees an Ident it's almost always a value.
  // But idents inside an expression context could be referencing a
  // type as a value (e.g. `T : type = i32`) — handle both.
  DefId def = query_resolve_ref(s, e, NS_VALUE);
  if (!def_id_is_valid(def))
    def = query_resolve_ref(s, e, NS_TYPE);
  if (!def_id_is_valid(def))
    return s->error_type;
  return query_type_of_def(s, def);
}

// Arithmetic bin op: numeric operands, result type chosen by
// "wider wins" with comptime int/float promoted to whatever the
// other side is. Same-shape comptime_int + comptime_int stays
// comptime_int so downstream coercion sees the comptime kind.
static struct Type *bin_arith_result(struct Sema *s, struct Type *l,
                                     struct Type *r, struct Expr *e) {
  if (!type_is_numeric(l) || !type_is_numeric(r)) {
    char lb[64], rb[64];
    diag_error(&s->diags, e->span,
               "binary operator '%s' requires numeric operands; "
               "got %s and %s",
               token_kind_to_str(e->bin.op),
               type_to_string(s, l, lb, sizeof(lb)),
               type_to_string(s, r, rb, sizeof(rb)));
    return s->error_type;
  }

  // comptime + comptime → comptime (preserve the comptime view so
  // downstream coercion can range-check against the concrete target).
  if (type_is_comptime(l) && type_is_comptime(r)) {
    if (l->kind == TY_COMPTIME_FLOAT || r->kind == TY_COMPTIME_FLOAT)
      return s->comptime_float_type;
    return s->comptime_int_type;
  }

  // comptime + concrete → concrete (the comptime side coerces in).
  // Validate via coerce — it'll range-check against any const value
  // we have. If the const isn't known, accept structurally.
  if (type_is_comptime(l) && !type_is_comptime(r)) {
    struct ConstValue lv = query_const_eval(s, e->bin.Left);
    if (!coerce(s, l, r, lv, e->bin.Left->span)) return s->error_type;
    return r;
  }
  if (!type_is_comptime(l) && type_is_comptime(r)) {
    struct ConstValue rv = query_const_eval(s, e->bin.Right);
    if (!coerce(s, r, l, rv, e->bin.Right->span)) return s->error_type;
    return l;
  }

  // concrete + concrete: must match exactly. No implicit widening.
  if (l != r) {
    char lb[64], rb[64];
    diag_error(&s->diags, e->span,
               "binary operator '%s': type mismatch — %s vs %s",
               token_kind_to_str(e->bin.op),
               type_to_string(s, l, lb, sizeof(lb)),
               type_to_string(s, r, rb, sizeof(rb)));
    return s->error_type;
  }
  return l;
}

static struct Type *bin_cmp_result(struct Sema *s, struct Type *l,
                                   struct Type *r, struct Expr *e) {
  // Comparison: operands must be compatible (same numeric kind,
  // or both bool, etc.). Result is bool.
  if (l->kind == TY_ERROR || r->kind == TY_ERROR) return s->error_type;

  bool ok = false;
  if (type_is_numeric(l) && type_is_numeric(r)) {
    // Same as arithmetic compatibility: comptime promotes to concrete,
    // concrete must match.
    if (type_is_comptime(l) || type_is_comptime(r)) {
      ok = true;
    } else {
      ok = (l == r);
    }
  } else if (l->kind == TY_BOOL && r->kind == TY_BOOL) {
    ok = (e->bin.op == EqualEqual || e->bin.op == BangEqual);
  }

  if (!ok) {
    char lb[64], rb[64];
    diag_error(&s->diags, e->span,
               "comparison '%s' between incompatible types %s and %s",
               token_kind_to_str(e->bin.op),
               type_to_string(s, l, lb, sizeof(lb)),
               type_to_string(s, r, rb, sizeof(rb)));
    return s->error_type;
  }
  return s->bool_type;
}

static struct Type *bin_logical_result(struct Sema *s, struct Type *l,
                                       struct Type *r, struct Expr *e) {
  if (l->kind != TY_BOOL || r->kind != TY_BOOL) {
    char lb[64], rb[64];
    diag_error(&s->diags, e->span,
               "logical '%s' requires bool operands; got %s and %s",
               token_kind_to_str(e->bin.op),
               type_to_string(s, l, lb, sizeof(lb)),
               type_to_string(s, r, rb, sizeof(rb)));
    return s->error_type;
  }
  return s->bool_type;
}

static struct Type *type_of_bin(struct Sema *s, struct Expr *e) {
  struct Type *l = query_type_of_expr(s, e->bin.Left);
  struct Type *r = query_type_of_expr(s, e->bin.Right);
  if (l->kind == TY_ERROR || r->kind == TY_ERROR) return s->error_type;

  switch (e->bin.op) {
  case Plus: case Minus: case Star: case ForwardSlash: case Percent:
  case StarStar: case Pipe: case Ampersand: case Caret:
  case ShiftLeft: case ShiftRight:
    return bin_arith_result(s, l, r, e);
  case EqualEqual: case BangEqual:
  case Less: case LessEqual: case Greater: case GreaterEqual:
    return bin_cmp_result(s, l, r, e);
  case AmpersandAmpersand: case PipePipe:
    return bin_logical_result(s, l, r, e);
  default:
    diag_error(&s->diags, e->span,
               "binary operator '%s' is not yet typed",
               token_kind_to_str(e->bin.op));
    return s->error_type;
  }
}

static struct Type *type_of_unary(struct Sema *s, struct Expr *e) {
  struct Type *t = query_type_of_expr(s, e->unary.operand);
  if (t->kind == TY_ERROR) return s->error_type;
  switch (e->unary.op) {
  case unary_Neg:
    if (!type_is_numeric(t)) {
      char b[64];
      diag_error(&s->diags, e->span,
                 "unary '-' requires a numeric operand; got %s",
                 type_to_string(s, t, b, sizeof(b)));
      return s->error_type;
    }
    return t;
  case unary_BitNot:
    if (!type_is_int(t)) {
      char b[64];
      diag_error(&s->diags, e->span,
                 "unary '~' requires an integer operand; got %s",
                 type_to_string(s, t, b, sizeof(b)));
      return s->error_type;
    }
    return t;
  case unary_Not:
    if (t->kind != TY_BOOL) {
      char b[64];
      diag_error(&s->diags, e->span,
                 "unary '!' requires a bool operand; got %s",
                 type_to_string(s, t, b, sizeof(b)));
      return s->error_type;
    }
    return s->bool_type;
  default:
    // Address-of, deref, pre/post-inc, type-position unaries — defer.
    return s->error_type;
  }
}

static struct Type *type_of_call(struct Sema *s, struct Expr *e) {
  struct Type *callee_t = query_type_of_expr(s, e->call.callee);
  if (callee_t->kind == TY_ERROR) return s->error_type;
  if (callee_t->kind != TY_FN) {
    char b[128];
    diag_error(&s->diags, e->call.callee->span,
               "call target is not a function; type is %s",
               type_to_string(s, callee_t, b, sizeof(b)));
    return s->error_type;
  }

  size_t arg_count = e->call.args ? e->call.args->count : 0;
  if (arg_count != callee_t->fn.param_count) {
    diag_error(&s->diags, e->span,
               "wrong number of arguments: expected %zu, got %zu",
               callee_t->fn.param_count, arg_count);
    return s->error_type;
  }

  for (size_t i = 0; i < arg_count; i++) {
    struct Expr **slot = (struct Expr **)vec_get(e->call.args, i);
    struct Expr *arg = slot ? *slot : NULL;
    if (!arg) continue;
    // Bidirectional check: arg synthesized type must coerce to the
    // expected param type. coerce() handles comptime → concrete.
    check_expr(s, arg, callee_t->fn.params[i]);
  }
  return callee_t->fn.ret;
}

static struct Type *type_of_lambda(struct Sema *s, struct Expr *e) {
  // Build the fn type from params + ret_type.
  size_t n = e->lambda.params ? e->lambda.params->count : 0;
  struct Type **params = NULL;
  if (n > 0) {
    params = arena_alloc(&s->arena, sizeof(struct Type *) * n);
    for (size_t i = 0; i < n; i++) {
      struct Param *p = (struct Param *)vec_get(e->lambda.params, i);
      if (!p || !p->type_ann) {
        params[i] = s->error_type;
        diag_error(&s->diags, e->span,
                   "function parameter #%zu requires a type annotation", i);
        continue;
      }
      params[i] = resolve_type_expr(s, p->type_ann);
    }
  }

  struct Type *ret = e->lambda.ret_type
                         ? resolve_type_expr(s, e->lambda.ret_type)
                         : s->void_type;

  struct Type *fn = type_fn(s, params, n, ret);

  // Body checking: synth body type, coerce to ret. NULL body = type
  // signature only (no body to check) — common in fn-typed signatures.
  if (e->lambda.body)
    check_expr(s, e->lambda.body, ret);

  return fn;
}

// Local bind inside a fn body: `x := 5`, `y : i32 = a + b`, etc.
//
// scope_walk's `define_local_bind` already allocated a DECL_USER
// DefId for this name in the enclosing scope, so name resolution
// works for downstream uses. Here we type-check the binding itself:
//
//   - With annotation: resolve the declared type, check the value
//     coerces to it, return the declared type.
//   - Without annotation: synthesize from the value, return that.
//
// The bind's *expression* type (what query_type_of_expr returns for
// the Bind node) is the type of its value — a Bind in expression
// position evaluates to the bound value, so a Block ending with a
// Bind has the bind's value as its type. (Some langs return void for
// `let` statements; we follow Zig and treat them as expressions.)
static struct Type *type_of_bind(struct Sema *s, struct Expr *e) {
  struct BindExpr *b = &e->bind;
  struct Expr *type_ann = b->type_ann;
  struct Expr *value = b->value;

  if (type_ann) {
    struct Type *declared = resolve_type_expr(s, type_ann);
    if (declared->kind == TY_ERROR) return declared;
    if (value)
      check_expr(s, value, declared);
    return declared;
  }
  return value ? query_type_of_expr(s, value) : s->void_type;
}

static struct Type *type_of_block(struct Sema *s, struct Expr *e) {
  // Type all statements (records deps, surfaces errors). Block
  // expression's type = type of the LAST statement, or void if empty.
  if (!e->block.stmts || e->block.stmts->count == 0)
    return s->void_type;

  struct Type *last = s->void_type;
  for (size_t i = 0; i < e->block.stmts->count; i++) {
    struct Expr **slot = (struct Expr **)vec_get(e->block.stmts, i);
    struct Expr *stmt = slot ? *slot : NULL;
    if (!stmt) continue;
    last = query_type_of_expr(s, stmt);
  }
  return last;
}

static struct Type *type_of_if(struct Sema *s, struct Expr *e) {
  // Condition must be bool.
  struct Type *cond = query_type_of_expr(s, e->if_expr.condition);
  if (cond->kind != TY_ERROR && cond->kind != TY_BOOL) {
    char b[64];
    diag_error(&s->diags, e->if_expr.condition->span,
               "if condition must be bool; got %s",
               type_to_string(s, cond, b, sizeof(b)));
  }

  struct Type *then_t = query_type_of_expr(s, e->if_expr.then_branch);
  if (!e->if_expr.else_branch) {
    // Statement-position if: type is void. (Strict expression-position
    // ifs require both branches; defer that distinction to later.)
    return s->void_type;
  }
  struct Type *else_t = query_type_of_expr(s, e->if_expr.else_branch);
  if (then_t->kind == TY_ERROR || else_t->kind == TY_ERROR)
    return s->error_type;
  if (then_t != else_t) {
    char tb[64], eb[64];
    diag_error(&s->diags, e->span,
               "if branches have incompatible types: then=%s, else=%s",
               type_to_string(s, then_t, tb, sizeof(tb)),
               type_to_string(s, else_t, eb, sizeof(eb)));
    return s->error_type;
  }
  return then_t;
}

static struct Type *type_of_index(struct Sema *s, struct Expr *e) {
  struct Type *obj = query_type_of_expr(s, e->index.object);
  struct Type *idx = query_type_of_expr(s, e->index.index);
  if (obj->kind == TY_ERROR || idx->kind == TY_ERROR) return s->error_type;
  if (!type_is_int(idx)) {
    char b[64];
    diag_error(&s->diags, e->index.index->span,
               "array index must be an integer; got %s",
               type_to_string(s, idx, b, sizeof(b)));
  }
  switch (obj->kind) {
  case TY_ARRAY: return obj->array.elem;
  case TY_SLICE: return obj->slice.elem;
  case TY_PTR:   return obj->ptr.elem;
  default: {
    char b[64];
    diag_error(&s->diags, e->index.object->span,
               "cannot index into non-array/slice type %s",
               type_to_string(s, obj, b, sizeof(b)));
    return s->error_type;
  }
  }
}

static struct Type *type_of_array_type_expr(struct Sema *s, struct Expr *e) {
  // `[N]T` / `[]T` / `[^]T` appearing in expression position is a
  // type-valued expression. Result type is `type` (the kind of types).
  // Validation flows through resolve_type_expr.
  (void)resolve_type_expr(s, e);
  return s->type_type ? s->type_type : s->error_type;
}
