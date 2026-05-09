#include "expr_check.h"

#include <stdio.h>

#include "../../common/arena.h"
#include "../../common/hashmap.h"
#include "../../common/stringpool.h"
#include "../../common/vec.h"
#include "../../diag/diag.h"
#include "../../parser/ast.h"
#include "../eval/const_eval.h"
#include "../modules/modules.h"
#include "../query/query_engine.h"
#include "../resolve/resolve.h"
#include "../resolve/scope_index.h"
#include "../scope/scope.h"
#include "../sema.h"
#include "checker.h"     // query_type_of_def, resolve_type_expr
#include "coerce.h"
#include "decl_data.h"
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
static struct Type *type_of_slice(struct Sema *s, struct Expr *e);
static struct Type *type_of_array_type_expr(struct Sema *s, struct Expr *e);
static struct Type *type_of_bind(struct Sema *s, struct Expr *e);
static struct Type *type_of_field(struct Sema *s, struct Expr *e);
static struct Type *type_of_product(struct Sema *s, struct Expr *e,
                                    struct Type *expected);
static struct Type *type_of_enum_ref(struct Sema *s, struct Expr *e,
                                     struct Type *expected);
static struct Type *type_of_return(struct Sema *s, struct Expr *e);
static struct Type *type_of_loop(struct Sema *s, struct Expr *e);
static struct Type *type_of_defer(struct Sema *s, struct Expr *e);
static struct Type *type_of_assign(struct Sema *s, struct Expr *e);
static struct Type *type_of_array_lit(struct Sema *s, struct Expr *e);
static struct Type *type_of_builtin(struct Sema *s, struct Expr *e);
static struct Type *type_of_switch(struct Sema *s, struct Expr *e);

// L-value test shared between expr_Assign and unary_Ref (address-of).
static bool target_is_assignable(struct Expr *t);

// Helper used by struct-literal + .Variant + field access for nicer
// diagnostics. Returns the interned name pointer or "?" on miss.
static const char *name_of(struct Sema *s, uint32_t name_id) {
  const char *n = pool_get(&s->pool, name_id, 0);
  return n ? n : "?";
}

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
  case expr_Slice:      result = type_of_slice(s, expr); break;
  case expr_ArrayType:
  case expr_SliceType:
  case expr_ManyPtrType:
    result = type_of_array_type_expr(s, expr);
    break;
  case expr_Bind:
    result = type_of_bind(s, expr);
    break;
  case expr_Field:
    result = type_of_field(s, expr);
    break;
  case expr_Product:
    // Synth-position: requires a type prefix. Bare `.{}` only types
    // in check-position; check_expr handles that before calling here.
    result = type_of_product(s, expr, /*expected=*/NULL);
    break;
  case expr_EnumRef:
    // Synth-position: errors. Bidirectional `.Variant` resolves only
    // when `check_expr` supplies an expected enum type.
    result = type_of_enum_ref(s, expr, /*expected=*/NULL);
    break;
  case expr_Return:
    result = type_of_return(s, expr);
    break;
  case expr_Break:
  case expr_Continue:
    // Loop-target validation (are we even inside a loop?) is a scope
    // concern, not a type concern. From the type system's perspective
    // these diverge — they never produce a value.
    result = s->noreturn_type;
    break;
  case expr_Loop:
    result = type_of_loop(s, expr);
    break;
  case expr_Defer:
    result = type_of_defer(s, expr);
    break;
  case expr_Assign:
    result = type_of_assign(s, expr);
    break;
  case expr_ArrayLit:
    result = type_of_array_lit(s, expr);
    break;
  case expr_Builtin:
    result = type_of_builtin(s, expr);
    break;
  case expr_Switch:
    result = type_of_switch(s, expr);
    break;
  default:
    // Switch arms (E.3+ patterns), Handler / Mask / Effect — all defer.
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
  // Bidirectional shapes: an expected type can supply context that
  // synth alone cannot. Cases for E.3:
  //   - expr_EnumRef (`.Variant`)   — synth always errors; check with
  //     a TY_ENUM expected resolves the variant.
  //   - expr_Product without prefix (`.{}`) — synth errors; check
  //     with a TY_STRUCT expected uses the expected struct.
  //   - expr_Block — propagate the expected type through to the last
  //     statement (the block's "result" stmt). Earlier stmts type
  //     in synth mode. This is what makes `fn() -> Color { .Green }`
  //     work — the return-type expectation from the enclosing lambda
  //     reaches `.Green` through the body block.
  if (expr && expected && expected->kind != TY_ERROR) {
    if (expr->kind == expr_EnumRef && expected->kind == TY_ENUM) {
      struct Type *t = type_of_enum_ref(s, expr, expected);
      return t && t->kind != TY_ERROR;
    }
    if (expr->kind == expr_Product && !expr->product.type_expr &&
        expected->kind == TY_STRUCT) {
      struct Type *t = type_of_product(s, expr, expected);
      return t && t->kind != TY_ERROR;
    }
    if (expr->kind == expr_Block && expr->block.stmts &&
        expr->block.stmts->count > 0) {
      // All but the last stmt: synth (record types, surface errors).
      // Last stmt: recurse with the expected type.
      size_t last = expr->block.stmts->count - 1;
      for (size_t i = 0; i < last; i++) {
        struct Expr **slot = (struct Expr **)vec_get(expr->block.stmts, i);
        struct Expr *stmt = slot ? *slot : NULL;
        if (stmt) (void)query_type_of_expr(s, stmt);
      }
      struct Expr **slot = (struct Expr **)vec_get(expr->block.stmts, last);
      struct Expr *tail = slot ? *slot : NULL;
      if (!tail) return false;
      return check_expr(s, tail, expected);
    }
  }

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
  case unary_Ref: {
    // `&x` — address-of. Operand must be an l-value. Result is `^T`,
    // a single (mutable) pointer. Const-ness is not inferred from the
    // binding today; users opt into `^const T` via type annotation
    // (`p :: ^const i32 = &x`).
    if (!target_is_assignable(e->unary.operand)) {
      diag_error(&s->diags, e->span,
                 "address-of requires an l-value (variable, field, "
                 "or index expression)");
      return s->error_type;
    }
    return type_ptr(s, t, /*is_const=*/false);
  }
  case unary_Deref: {
    // `x^` — postfix deref. Reads through a single pointer. Many-
    // pointers (`[^]T`) deref via indexing (`p[0]`) since they have
    // no inherent length-of-1 semantics; rejecting them here mirrors
    // Zig (`*T` and `[*]T` are distinct in what they support).
    if (t->kind != TY_PTR) {
      char b[64];
      diag_error(&s->diags, e->span,
                 "deref '^' requires a single pointer ^T; got %s",
                 type_to_string(s, t, b, sizeof(b)));
      return s->error_type;
    }
    return t->ptr.elem;
  }
  default:
    // Pre/post-inc, type-position unaries (Ptr/ManyPtr/Const/Optional/
    // DeNil — these belong in `resolve_type_expr`, not here) — defer.
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
  // Zig-strict: only TY_ARRAY / TY_SLICE / TY_MANY_PTR are indexable.
  // A single-pointer `^T` points at exactly one element — to read it
  // you deref (`p^`), not index. `[^]T` is the many-pointer type that
  // permits pointer arithmetic and indexing.
  switch (obj->kind) {
  case TY_ARRAY:    return obj->array.elem;
  case TY_SLICE:    return obj->slice.elem;
  case TY_MANY_PTR: return obj->many_ptr.elem;
  default: {
    char b[64];
    diag_error(&s->diags, e->index.object->span,
               "cannot index into %s (use a many-pointer [^]T or "
               "deref a single pointer)",
               type_to_string(s, obj, b, sizeof(b)));
    return s->error_type;
  }
  }
}

// Slice operation `arr[start..end]` / `arr[start..]` / `arr[..end]`.
//
// Type rules mirror Zig (Sema.zig:30769-30847, `analyzeSlice`):
//   - Bounds must coerce to `usize` (we accept any int kind for E.3.5c;
//     full strictness is a follow-up).
//   - The element type comes from the receiver:
//        TY_ARRAY → obj->array.elem
//        TY_SLICE → obj->slice.elem
//        TY_PTR   → obj->ptr.elem (operating on a many-pointer; we don't
//                   distinguish ^T vs [^]T at the type level today)
//   - When BOTH start and end const-eval to ints, the resulting type is
//     `^[N]T` (single-pointer-to-array of known length). This matches
//     Zig: comptime-known bounds preserve length-in-type information,
//     enabling array.len-style optimizations downstream.
//   - Otherwise (any bound runtime-only or missing) the result is `[]T`.
//
// Open-ended forms:
//   `arr[start..]` — end is implicitly the receiver's length. For
//     comptime-known start + comptime-known receiver length (TY_ARRAY)
//     we can still produce ^[N-start]T. For other cases, []T.
//   `arr[..end]` — same shape with start = 0.
static struct Type *type_of_slice(struct Sema *s, struct Expr *e) {
  struct Type *obj = query_type_of_expr(s, e->slice.object);
  if (obj->kind == TY_ERROR) return s->error_type;

  // Zig-strict: only TY_ARRAY / TY_SLICE / TY_MANY_PTR are sliceable.
  // Slicing a `^T` (single pointer to one element) makes no sense —
  // there's no length to slice from.
  struct Type *elem = NULL;
  switch (obj->kind) {
  case TY_ARRAY:    elem = obj->array.elem;    break;
  case TY_SLICE:    elem = obj->slice.elem;    break;
  case TY_MANY_PTR: elem = obj->many_ptr.elem; break;
  default: {
    char b[64];
    diag_error(&s->diags, e->slice.object->span,
               "cannot slice %s (slicing requires an array, slice, or "
               "many-pointer [^]T)",
               type_to_string(s, obj, b, sizeof(b)));
    return s->error_type;
  }
  }

  // Bound type-checks: both bounds must be integer-typed when present.
  // Const-eval each bound so we can decide between `^[N]T` and `[]T`.
  bool start_known = false, end_known = false;
  int64_t start_val = 0, end_val = 0;

  if (e->slice.start) {
    struct Type *st = query_type_of_expr(s, e->slice.start);
    if (st->kind != TY_ERROR && !type_is_int(st)) {
      char b[64];
      diag_error(&s->diags, e->slice.start->span,
                 "slice start must be an integer; got %s",
                 type_to_string(s, st, b, sizeof(b)));
    }
    struct ConstValue cv = query_const_eval(s, e->slice.start);
    if (cv.kind == CONST_INT) { start_known = true; start_val = cv.int_val; }
  } else {
    // Implicit start = 0 for `arr[..end]`.
    start_known = true;
    start_val = 0;
  }

  if (e->slice.end) {
    struct Type *et = query_type_of_expr(s, e->slice.end);
    if (et->kind != TY_ERROR && !type_is_int(et)) {
      char b[64];
      diag_error(&s->diags, e->slice.end->span,
                 "slice end must be an integer; got %s",
                 type_to_string(s, et, b, sizeof(b)));
    }
    struct ConstValue cv = query_const_eval(s, e->slice.end);
    if (cv.kind == CONST_INT) { end_known = true; end_val = cv.int_val; }
  } else if (obj->kind == TY_ARRAY) {
    // Implicit end = array length for `arr[start..]` on a fixed array.
    end_known = true;
    end_val = (int64_t)obj->array.size;
  }

  // Comptime-known bounds → ^[N]T. Otherwise []T.
  if (start_known && end_known && end_val >= start_val) {
    uint64_t n = (uint64_t)(end_val - start_val);
    return type_ptr(s, type_array(s, elem, n), /*is_const=*/false);
  }
  return type_slice(s, elem, /*is_const=*/false);
}

static struct Type *type_of_array_type_expr(struct Sema *s, struct Expr *e) {
  // `[N]T` / `[]T` / `[^]T` appearing in expression position is a
  // type-valued expression. Result type is `type` (the kind of types).
  // Validation flows through resolve_type_expr.
  (void)resolve_type_expr(s, e);
  return s->type_type ? s->type_type : s->error_type;
}

// =====================================================================
// E.3: Field access, struct literal, .Variant
// =====================================================================

// Linear search for a field by name in a flat FieldData arena. Returns
// the index on hit, SIZE_MAX on miss. Linear because for realistic
// structs (<32 fields) the cache locality wins; if we ever care, we
// can add a per-signature name->index hashmap.
static size_t struct_find_field(struct StructSignature *sig, uint32_t name_id) {
  for (size_t i = 0; i < sig->field_count; i++) {
    if (sig->fields[i].name_id == name_id) return i;
  }
  return (size_t)-1;
}

static size_t enum_find_variant(struct EnumSignature *sig, uint32_t name_id) {
  for (size_t i = 0; i < sig->variant_count; i++) {
    if (sig->variants[i].name_id == name_id) return i;
  }
  return (size_t)-1;
}

// Field access — `obj.field`. The receiver's type drives the lookup:
//
//   TY_STRUCT(def)  → query_struct_signature → linear search by name.
//                     Anonymous union arms live in the same flat arena
//                     under non-zero union_group, so `obj.x` resolves
//                     to the arm directly without walking unions.
//   TY_ENUM(def)    → query_enum_signature → variant lookup. Result
//                     type is the enum itself (a value of `Color.Red`
//                     has type `Color`).
//   TY_SLICE        → builtin `.ptr` (yields `^T` / `^const T`) and
//                     `.len` (yields `usize`). Mirrors Zig's slice ABI:
//                     the runtime layout is `(ptr, len)`, but they're
//                     surfaced as compiler-synthesized fields, not
//                     struct fields.
//   TY_ARRAY        → builtin `.len` (yields `comptime_int` since the
//                     length is part of the type).
//
// Anything else is an error.
static bool name_is(struct Sema *s, uint32_t id, const char *lit) {
  const char *got = pool_get(&s->pool, id, 0);
  if (!got) return false;
  for (size_t i = 0; ; i++) {
    if (got[i] != lit[i]) return false;
    if (lit[i] == '\0') return true;
  }
}

static struct Type *type_of_field(struct Sema *s, struct Expr *e) {
  struct Type *recv = query_type_of_expr(s, e->field.object);
  if (recv->kind == TY_ERROR) return s->error_type;

  uint32_t fname = e->field.field.string_id;

  if (recv->kind == TY_STRUCT) {
    struct StructSignature *sig = query_struct_signature(s, recv->struct_.def);
    if (!sig) return s->error_type;
    size_t idx = struct_find_field(sig, fname);
    if (idx == (size_t)-1) {
      char rb[64];
      diag_error(&s->diags, e->field.field.span,
                 "no field '%s' on %s",
                 name_of(s, fname),
                 type_to_string(s, recv, rb, sizeof(rb)));
      return s->error_type;
    }

    // Visibility: a private field is accessible only from inside the
    // struct's owning module. Cross-module access requires Visibility_public.
    if (!visibility_allows_external(sig->fields[idx].vis)) {
      struct DefInfo *struct_di = def_info(s, recv->struct_.def);
      ScopeId owner_scope =
          struct_di ? struct_di->owner_scope : SCOPE_ID_INVALID;
      struct ScopeInfo *si =
          scope_id_is_valid(owner_scope) ? scope_info(s, owner_scope) : NULL;
      ModuleId struct_module = si ? si->owner_module : MODULE_ID_INVALID;
      ModuleId access_module = module_for_span(s, e->field.field.span);
      if (module_id_is_valid(struct_module) &&
          module_id_is_valid(access_module) &&
          !module_id_eq(struct_module, access_module)) {
        char rb[64];
        diag_error(&s->diags, e->field.field.span,
                   "field '%s' on %s is private",
                   name_of(s, fname),
                   type_to_string(s, recv, rb, sizeof(rb)));
      }
    }

    return sig->fields[idx].type;
  }

  if (recv->kind == TY_ENUM) {
    struct EnumSignature *sig = query_enum_signature(s, recv->enum_.def);
    if (!sig) return s->error_type;
    size_t idx = enum_find_variant(sig, fname);
    if (idx == (size_t)-1) {
      char rb[64];
      diag_error(&s->diags, e->field.field.span,
                 "no variant '%s' in %s",
                 name_of(s, fname),
                 type_to_string(s, recv, rb, sizeof(rb)));
      return s->error_type;
    }
    return recv;  // variant access yields a value of the enum type
  }

  if (recv->kind == TY_SLICE) {
    // Mirrors Zig's slice ABI (Sema.zig:30088 `analyzeSlicePtr`):
    // `slice.ptr` yields a many-pointer `[^]T` (with the slice's
    // const-ness preserved), NOT a single pointer `^T`. Many-pointers
    // permit pointer arithmetic, which is what slice consumers
    // typically need.
    if (name_is(s, fname, "ptr"))
      return type_many_ptr(s, recv->slice.elem, recv->slice.is_const);
    if (name_is(s, fname, "len"))
      return s->usize_type;
    diag_error(&s->diags, e->field.field.span,
               "no field '%s' on slice (only 'ptr' and 'len' are valid)",
               name_of(s, fname));
    return s->error_type;
  }

  if (recv->kind == TY_ARRAY) {
    // `array.len` returns `usize`. The value is comptime-known (the
    // length is part of the type), but the type system surfaces it as
    // `usize` to keep array.len and slice.len uniformly assignable to
    // the same target. Mirrors Zig (Sema.zig:25513).
    if (name_is(s, fname, "len"))
      return s->usize_type;
    diag_error(&s->diags, e->field.field.span,
               "no field '%s' on array (only 'len' is valid)",
               name_of(s, fname));
    return s->error_type;
  }

  char b[64];
  diag_error(&s->diags, e->field.object->span,
             "field access on non-struct/enum type %s",
             type_to_string(s, recv, b, sizeof(b)));
  return s->error_type;
}

// Struct literal — `Point.{ .x = 0, .y = 0 }` or bare `.{}` when in
// check-position with an expected struct type. Validation:
//   - Each provided field name must exist on the struct.
//   - Each provided field's value must coerce to the field's type.
//   - All required fields (today: every field) must be provided. Field
//     defaults exist on FieldData but aren't auto-filled in E.3 — once
//     we have a model for "fields with defaults are optional," wire it.
static struct Type *type_of_product(struct Sema *s, struct Expr *e,
                                    struct Type *expected) {
  // Resolve the target struct type. Two routes: explicit type prefix
  // (`Point.{...}`) or bidirectional context (`.{...}` with a TY_STRUCT
  // expected).
  struct Type *target = NULL;
  if (e->product.type_expr) {
    target = resolve_type_expr(s, e->product.type_expr);
    if (!target || target->kind == TY_ERROR) return s->error_type;
  } else if (expected && expected->kind == TY_STRUCT) {
    target = expected;
  } else {
    diag_error(&s->diags, e->span,
               "anonymous struct literal '.{ ... }' requires a type "
               "prefix or an expected struct type");
    return s->error_type;
  }

  if (target->kind != TY_STRUCT) {
    char b[64];
    diag_error(&s->diags, e->span,
               "struct literal expects a struct type; got %s",
               type_to_string(s, target, b, sizeof(b)));
    return s->error_type;
  }

  struct StructSignature *sig = query_struct_signature(s, target->struct_.def);
  if (!sig) return s->error_type;

  Vec *provided = e->product.Fields;
  size_t pn = provided ? provided->count : 0;

  // Track which signature fields have been provided — used to flag
  // missing required ones. Stack-allocate up to a reasonable bound;
  // arena-fall-back if larger.
  bool stack_seen[64] = {0};
  bool *seen = sig->field_count <= 64
                   ? stack_seen
                   : arena_alloc(&s->arena, sizeof(bool) * sig->field_count);
  if (seen != stack_seen)
    for (size_t i = 0; i < sig->field_count; i++) seen[i] = false;

  for (size_t i = 0; i < pn; i++) {
    struct ProductField *pf = (struct ProductField *)vec_get(provided, i);
    if (!pf) continue;
    size_t idx = struct_find_field(sig, pf->name.string_id);
    if (idx == (size_t)-1) {
      char tb[64];
      diag_error(&s->diags, pf->name.span,
                 "no field '%s' on %s",
                 name_of(s, pf->name.string_id),
                 type_to_string(s, target, tb, sizeof(tb)));
      continue;
    }
    seen[idx] = true;
    if (pf->value)
      check_expr(s, pf->value, sig->fields[idx].type);
  }

  // Missing required fields. Every standalone field (union_group == 0)
  // must be initialized; the union-group check below handles arms.
  for (size_t i = 0; i < sig->field_count; i++) {
    if (sig->fields[i].union_group != 0) continue;
    if (!seen[i]) {
      char tb[64];
      diag_error(&s->diags, e->span,
                 "missing field '%s' in literal of %s",
                 name_of(s, sig->fields[i].name_id),
                 type_to_string(s, target, tb, sizeof(tb)));
    }
  }
  // Walk again to verify each union group has at least one arm set.
  // This is O(field_count^2) in the worst case but field_count is
  // tiny; not worth a separate pass index.
  for (size_t i = 0; i < sig->field_count; i++) {
    uint32_t g = sig->fields[i].union_group;
    if (g == 0) continue;
    // Was this group satisfied by some arm earlier in the iteration?
    bool group_seen = false;
    for (size_t j = 0; j < sig->field_count; j++) {
      if (sig->fields[j].union_group == g && seen[j]) {
        group_seen = true;
        break;
      }
    }
    if (!group_seen) {
      char tb[64];
      diag_error(&s->diags, e->span,
                 "anonymous union in %s requires one arm to be initialized",
                 type_to_string(s, target, tb, sizeof(tb)));
      // Skip the rest of this group's iterations.
      while (i + 1 < sig->field_count && sig->fields[i + 1].union_group == g)
        i++;
    } else {
      while (i + 1 < sig->field_count && sig->fields[i + 1].union_group == g)
        i++;
    }
  }

  return target;
}

// `.Variant` enum-literal. Synth: error. Check w/ TY_ENUM expected:
// look up the variant in the enum's signature and yield the enum type.
static struct Type *type_of_enum_ref(struct Sema *s, struct Expr *e,
                                     struct Type *expected) {
  if (!expected || expected->kind != TY_ENUM) {
    diag_error(&s->diags, e->span,
               "enum literal '.%s' requires an enum-typed context",
               name_of(s, e->enum_ref_expr.name.string_id));
    return s->error_type;
  }

  struct EnumSignature *sig = query_enum_signature(s, expected->enum_.def);
  if (!sig) return s->error_type;
  size_t idx = enum_find_variant(sig, e->enum_ref_expr.name.string_id);
  if (idx == (size_t)-1) {
    char tb[64];
    diag_error(&s->diags, e->span,
               "no variant '%s' in %s",
               name_of(s, e->enum_ref_expr.name.string_id),
               type_to_string(s, expected, tb, sizeof(tb)));
    return s->error_type;
  }
  return expected;
}

// =====================================================================
// E.3.5a — control flow + assign + array literal handlers
// =====================================================================

// `return expr` — validate the value coerces to the enclosing fn's
// return type. The enclosing fn is recovered via the per-module
// node→decl index (query_node_to_decl); from there, query_fn_signature
// gives us the declared ret_type. The expression itself diverges, so
// its own type is `noreturn` regardless of validity.
static struct Type *type_of_return(struct Sema *s, struct Expr *e) {
  DefId fn_def = query_node_to_decl(s, e->id);
  if (def_id_is_valid(fn_def)) {
    struct FnSignature *sig = query_fn_signature(s, fn_def);
    if (sig && sig->ret_type) {
      if (e->return_expr.value)
        check_expr(s, e->return_expr.value, sig->ret_type);
      else if (sig->ret_type->kind != TY_VOID) {
        char rb[64];
        diag_error(&s->diags, e->span,
                   "return without a value in fn returning %s",
                   type_to_string(s, sig->ret_type, rb, sizeof(rb)));
      }
    }
  } else if (e->return_expr.value) {
    // Outside a fn (e.g. top-level return); still type the operand
    // so any inner errors surface, but no signature to check against.
    (void)query_type_of_expr(s, e->return_expr.value);
  }
  return s->noreturn_type;
}

// `Loop { ... }` — body types in synth, loop expression is statement-
// like and yields `void`. Break-with-value (labeled-break) is a future
// feature; when it lands the loop's type joins the break payloads.
static struct Type *type_of_loop(struct Sema *s, struct Expr *e) {
  if (e->loop_expr.init)      (void)query_type_of_expr(s, e->loop_expr.init);
  if (e->loop_expr.condition) {
    struct Type *ct = query_type_of_expr(s, e->loop_expr.condition);
    if (ct->kind != TY_BOOL && ct->kind != TY_ERROR) {
      char b[64];
      diag_error(&s->diags, e->loop_expr.condition->span,
                 "loop condition must be bool; got %s",
                 type_to_string(s, ct, b, sizeof(b)));
    }
  }
  if (e->loop_expr.step) (void)query_type_of_expr(s, e->loop_expr.step);
  if (e->loop_expr.body) (void)query_type_of_expr(s, e->loop_expr.body);
  return s->void_type;
}

// `defer expr` — body types unconditionally; the defer itself yields
// `void`. The body's type doesn't matter: defer runs for side effects.
static struct Type *type_of_defer(struct Sema *s, struct Expr *e) {
  if (e->defer_expr.value)
    (void)query_type_of_expr(s, e->defer_expr.value);
  return s->void_type;
}

// `target = value` — validate target is assignable (l-value-ish) and
// value coerces to target's type. Assignments are statement-like; the
// expression yields `void`. We don't have a formal lvalue category yet;
// instead reject obvious non-assignable targets (literals, calls, etc.)
// and let the rest pass.
static bool target_is_assignable(struct Expr *t) {
  if (!t) return false;
  switch (t->kind) {
  case expr_Ident:
  case expr_Field:
  case expr_Index:
    return true;
  case expr_Unary:
    // `^x = ...` — pointer dereference is an l-value.
    return t->unary.op == unary_Deref;
  default:
    return false;
  }
}

static struct Type *type_of_assign(struct Sema *s, struct Expr *e) {
  if (!target_is_assignable(e->assign.target)) {
    diag_error(&s->diags,
               e->assign.target ? e->assign.target->span : e->span,
               "assignment target is not assignable");
    if (e->assign.value) (void)query_type_of_expr(s, e->assign.value);
    return s->void_type;
  }
  struct Type *target_t = query_type_of_expr(s, e->assign.target);
  if (e->assign.value)
    check_expr(s, e->assign.value, target_t);
  return s->void_type;
}

// `[N]T{e0, e1, ...}` — element type comes from the type prefix; each
// element coerces to it; total count must match N. Inferred-size form
// (`[_]T{...}`) infers N from the element count.
static struct Type *type_of_array_lit(struct Sema *s, struct Expr *e) {
  // Element type — required for E.3.5a (no full inference yet).
  struct Type *elem_t = NULL;
  if (e->array_lit.elem_type) {
    elem_t = resolve_type_expr(s, e->array_lit.elem_type);
    if (!elem_t || elem_t->kind == TY_ERROR) return s->error_type;
  } else {
    diag_error(&s->diags, e->span,
               "array literal requires an explicit element type");
    return s->error_type;
  }

  // Initializer is a Product whose Fields hold the elements (positional).
  struct Expr *init = e->array_lit.initializer;
  Vec *elems = (init && init->kind == expr_Product) ? init->product.Fields : NULL;
  size_t n = elems ? elems->count : 0;

  // Count: explicit size must match; inferred size adopts n.
  uint64_t count = (uint64_t)n;
  if (!e->array_lit.size_inferred && e->array_lit.size) {
    struct ConstValue size_val = query_const_eval(s, e->array_lit.size);
    if (size_val.kind != CONST_INT || size_val.int_val < 0) {
      diag_error(&s->diags, e->array_lit.size->span,
                 "array size must be a non-negative comptime integer");
      return s->error_type;
    }
    count = (uint64_t)size_val.int_val;
    if ((uint64_t)n != count) {
      diag_error(&s->diags, e->span,
                 "array literal has %zu elements but type declares %llu",
                 n, (unsigned long long)count);
    }
  }

  // Type-check each element against the declared elem type.
  for (size_t i = 0; i < n; i++) {
    struct ProductField *pf = (struct ProductField *)vec_get(elems, i);
    if (pf && pf->value) check_expr(s, pf->value, elem_t);
  }

  return type_array(s, elem_t, count);
}

// =====================================================================
// E.3.5b — compile-time builtin functions (`@sizeOf`, `@TypeOf`, etc.)
// =====================================================================
//
// Builtins land in the AST as `expr_Builtin { name_id, args }`. We
// dispatch by the (already-interned) name_id and validate per-builtin
// arity + argument shape. Mirrors Zig's per-builtin handler in
// Sema.zig (e.g., `zirSizeOf`, `zirTypeof`, `zirTypeName`,
// `zirAlignOf`, `zirIntCast`).
//
// Argument convention follows Zig:
//   `@sizeOf(T)`     — T is a type expression; returns comptime_int
//   `@alignOf(T)`    — T is a type expression; returns comptime_int
//   `@TypeOf(x)`     — x is a value expression; returns `type`
//   `@typeName(T)`   — T is a type expression; returns `string`
//   `@intCast(T, x)` — T is a type expression, x is a value; returns T
//
// Arg count + arg shape errors emit a diagnostic and return error_type.
// The result type is a real Type* in success paths.

static struct Expr *builtin_arg(struct Expr *e, size_t i) {
  if (!e->builtin.args || i >= e->builtin.args->count) return NULL;
  struct Expr **slot = (struct Expr **)vec_get(e->builtin.args, i);
  return slot ? *slot : NULL;
}

static bool require_arg_count(struct Sema *s, struct Expr *e, size_t want) {
  size_t got = e->builtin.args ? e->builtin.args->count : 0;
  if (got == want) return true;
  diag_error(&s->diags, e->span,
             "@%s expects %zu argument%s, got %zu",
             name_of(s, e->builtin.name_id), want,
             want == 1 ? "" : "s", got);
  return false;
}

static struct Type *type_of_builtin(struct Sema *s, struct Expr *e) {
  uint32_t nm = e->builtin.name_id;

  if (nm == s->name_sizeOf || nm == s->name_alignOf) {
    if (!require_arg_count(s, e, 1)) return s->error_type;
    struct Type *t = resolve_type_expr(s, builtin_arg(e, 0));
    if (!t || t->kind == TY_ERROR) return s->error_type;
    return s->comptime_int_type;
  }

  if (nm == s->name_TypeOf) {
    if (!require_arg_count(s, e, 1)) return s->error_type;
    // Synth the operand's type. Side-effect suppression (Zig's
    // `is_typeof` flag) is overkill for E.3.5b — we have no
    // call-evaluation in the typecheck pass, so synth never executes.
    struct Type *t = query_type_of_expr(s, builtin_arg(e, 0));
    if (!t || t->kind == TY_ERROR) return s->error_type;
    return s->type_type;
  }

  if (nm == s->name_typeName) {
    if (!require_arg_count(s, e, 1)) return s->error_type;
    struct Type *t = resolve_type_expr(s, builtin_arg(e, 0));
    if (!t || t->kind == TY_ERROR) return s->error_type;
    return s->string_type;
  }

  if (nm == s->name_intCast) {
    if (!require_arg_count(s, e, 2)) return s->error_type;
    struct Type *target = resolve_type_expr(s, builtin_arg(e, 0));
    if (!target || target->kind == TY_ERROR) return s->error_type;
    if (!type_is_int(target)) {
      char b[64];
      diag_error(&s->diags, builtin_arg(e, 0)->span,
                 "@intCast target must be an integer type; got %s",
                 type_to_string(s, target, b, sizeof(b)));
      return s->error_type;
    }
    // Operand must be a numeric value; we don't fold here — the
    // operand's type is checked, and any further safety/range checks
    // happen at codegen / runtime.
    struct Type *src_t = query_type_of_expr(s, builtin_arg(e, 1));
    if (!src_t || src_t->kind == TY_ERROR) return s->error_type;
    if (!type_is_int(src_t)) {
      char b[64];
      diag_error(&s->diags, builtin_arg(e, 1)->span,
                 "@intCast operand must be an integer; got %s",
                 type_to_string(s, src_t, b, sizeof(b)));
      return s->error_type;
    }
    return target;
  }

  diag_error(&s->diags, e->span,
             "unknown builtin '@%s'", name_of(s, nm));
  return s->error_type;
}

// =====================================================================
// switch — pattern dispatch (unit-variant enums + integer literals)
// =====================================================================
//
// Today's coverage is a slice of full pattern matching (E.4 territory):
//
//   - Scrutinee must be TY_ENUM or an integer kind (incl. comptime_int).
//   - Patterns supported:
//        `.Variant`     — for enum scrutinees, matches that variant
//        integer literal — for int scrutinees, matches that value
//        `_`            — wildcard, matches anything
//   - Multiple patterns per arm via `|`.
//   - All arm bodies must produce the same type. The first arm's body
//     type sets the expectation; subsequent arms are checked against
//     it. The whole switch expression's type is that joined type.
//   - Exhaustiveness: enum scrutinees require every variant covered
//     (or a wildcard); int scrutinees require a wildcard. Failure is
//     diagnosed but the switch still types so downstream checking can
//     continue.
//
// Defers to E.4: struct/tuple destructuring, range patterns, guards,
// nested patterns, capture binders.

static bool pattern_matches_enum_variant(struct Sema *s, struct Expr *pat,
                                         struct EnumSignature *sig,
                                         size_t *out_idx) {
  if (!pat || !sig) return false;
  if (pat->kind != expr_EnumRef) return false;
  uint32_t name = pat->enum_ref_expr.name.string_id;
  for (size_t i = 0; i < sig->variant_count; i++) {
    if (sig->variants[i].name_id == name) {
      if (out_idx) *out_idx = i;
      return true;
    }
  }
  return false;
}

static struct Type *type_of_switch(struct Sema *s, struct Expr *e) {
  struct Type *scrut = query_type_of_expr(s, e->switch_expr.scrutinee);
  if (scrut->kind == TY_ERROR) return s->error_type;

  bool is_enum = scrut->kind == TY_ENUM;
  bool is_int  = type_is_int(scrut);
  if (!is_enum && !is_int) {
    char b[64];
    diag_error(&s->diags, e->switch_expr.scrutinee->span,
               "switch scrutinee must be an enum or integer; got %s",
               type_to_string(s, scrut, b, sizeof(b)));
    return s->error_type;
  }

  // For enum exhaustiveness, mark which variants are covered.
  struct EnumSignature *enum_sig = NULL;
  bool stack_seen[64] = {0};
  bool *seen = NULL;
  size_t variant_count = 0;
  if (is_enum) {
    enum_sig = query_enum_signature(s, scrut->enum_.def);
    if (!enum_sig) return s->error_type;
    variant_count = enum_sig->variant_count;
    seen = variant_count <= 64
               ? stack_seen
               : arena_alloc(&s->arena, sizeof(bool) * variant_count);
    if (seen != stack_seen)
      for (size_t i = 0; i < variant_count; i++) seen[i] = false;
  }
  bool has_wildcard = false;

  Vec *arms = e->switch_expr.arms;
  size_t n = arms ? arms->count : 0;
  if (n == 0) {
    diag_error(&s->diags, e->span, "switch must have at least one arm");
    return s->error_type;
  }

  // First arm sets the result type expectation; subsequent arms must
  // match. Synth-mode for now (no bidirectional check from the switch's
  // surrounding context). E.4 can extend with a `check`-style entry.
  struct Type *result = NULL;

  for (size_t i = 0; i < n; i++) {
    struct SwitchArm *arm = (struct SwitchArm *)vec_get(arms, i);
    if (!arm) continue;

    // Validate each pattern in the arm.
    Vec *pats = arm->patterns;
    size_t pn = pats ? pats->count : 0;
    for (size_t j = 0; j < pn; j++) {
      struct Expr **pslot = (struct Expr **)vec_get(pats, j);
      struct Expr *pat = pslot ? *pslot : NULL;
      if (!pat) continue;

      if (pat->kind == expr_Wildcard) {
        has_wildcard = true;
        continue;
      }

      if (is_enum) {
        size_t vidx;
        if (pattern_matches_enum_variant(s, pat, enum_sig, &vidx)) {
          if (seen[vidx]) {
            diag_error(&s->diags, pat->span,
                       "duplicate variant '%s' in switch",
                       name_of(s, enum_sig->variants[vidx].name_id));
          }
          seen[vidx] = true;
        } else {
          char sb[64];
          diag_error(&s->diags, pat->span,
                     "pattern is not a variant of %s",
                     type_to_string(s, scrut, sb, sizeof(sb)));
        }
      } else {
        // Integer scrutinee — pattern must be an integer literal (or
        // const-foldable to one). const_eval handles both shapes
        // uniformly.
        struct ConstValue cv = query_const_eval(s, pat);
        if (cv.kind != CONST_INT) {
          diag_error(&s->diags, pat->span,
                     "switch arm pattern must be an integer literal");
        }
      }
    }

    // Type the arm body. First arm sets `result`; later arms join.
    if (arm->body) {
      struct Type *body_t = query_type_of_expr(s, arm->body);
      if (body_t->kind == TY_ERROR) {
        if (!result) result = s->error_type;
        continue;
      }
      if (!result) {
        result = body_t;
      } else if (result != body_t && result->kind != TY_ERROR &&
                 body_t->kind != TY_ERROR) {
        // Strict-equality join (Zig style). Mismatched arms error;
        // future work can promote comptime → concrete or unify.
        char rb[64], bb[64];
        diag_error(&s->diags, arm->body->span,
                   "switch arm body type %s does not match earlier arm "
                   "type %s",
                   type_to_string(s, body_t, bb, sizeof(bb)),
                   type_to_string(s, result, rb, sizeof(rb)));
      }
    }
  }

  // Exhaustiveness.
  if (!has_wildcard) {
    if (is_enum) {
      for (size_t i = 0; i < variant_count; i++) {
        if (!seen[i]) {
          char sb[64];
          diag_error(&s->diags, e->span,
                     "non-exhaustive switch on %s: missing variant '%s'",
                     type_to_string(s, scrut, sb, sizeof(sb)),
                     name_of(s, enum_sig->variants[i].name_id));
        }
      }
    } else {
      diag_error(&s->diags, e->span,
                 "switch on integer requires a wildcard '_' arm "
                 "(can't enumerate every value)");
    }
  }

  return result ? result : s->error_type;
}
