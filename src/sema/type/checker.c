#include "checker.h"

#include <stdio.h>

#include "../../common/stringpool.h"
#include "../../diag/diag.h"
#include "../../parser/ast.h"
#include "../eval/const_eval.h"
#include "../query/query_engine.h"
#include "../resolve/resolve.h"
#include "../sema.h"
#include "decl_info.h"
#include "fits.h"
#include "type.h"

// Resolve a type-position Expr* to a Type*. Handles the bare
// primitive Ident case ("u8", "f32", etc.); unknown shapes return
// the error type with a diagnostic.
static struct Type *resolve_type_expr(struct Sema *s, struct Expr *e) {
  if (!e) return s->error_type;

  if (e->kind == expr_Ident) {
    DefId def = query_resolve_ref(s, e, NS_TYPE);
    if (!def_id_is_valid(def)) {
      // resolve already emitted "name not found" — just return error
      return s->error_type;
    }
    struct DefInfo *di = def_info(s, def);
    if (!di || di->kind != DECL_PRIMITIVE) {
      diag_error(&s->diags, e->span,
                 "type position only accepts primitive types in Stage E.1");
      return s->error_type;
    }
    struct Type *t = type_for_primitive_name(s, di->name_id);
    if (!t) {
      diag_error(&s->diags, e->span,
                 "no Type registered for primitive '%s'",
                 pool_get(&s->pool, di->name_id, 0));
      return s->error_type;
    }
    return t;
  }

  diag_error(&s->diags, e->span,
             "type expressions other than primitive idents are not yet "
             "supported");
  return s->error_type;
}

// Inferred type for a Bind without annotation. Today: pure literal
// shape — int / float / nothing-known. Bigger Bind shapes (Lambda,
// Struct, Enum) become real types in later stages.
static struct Type *infer_type_from_value(struct Sema *s, struct Expr *value) {
  if (!value) return s->error_type;
  struct ConstValue v = query_const_eval(s, value);
  switch (v.kind) {
  case CONST_INT:   return s->comptime_int_type;
  case CONST_FLOAT: return s->comptime_float_type;
  case CONST_NONE:  return s->error_type;
  }
  return s->error_type;
}

// Range-check a const value against an annotated type. Emits a
// diagnostic with the offending value + range bounds on miss. Does
// NOT emit on CONST_NONE (the const_eval layer already flagged that
// path with its own diag — overflow during evaluation, etc.).
static void check_value_fits(struct Sema *s, struct Expr *value_expr,
                             struct ConstValue v, struct Type *t) {
  if (v.kind == CONST_NONE) return;
  if (!t || t->kind == TY_ERROR) return;

  const char *lo = NULL, *hi = NULL;
  if (fits_in(v, t, &lo, &hi)) return;

  char vbuf[64];
  const_value_to_str(v, vbuf, sizeof(vbuf));

  if (lo && hi) {
    diag_error(&s->diags, value_expr->span,
               "value %s does not fit in %s (range %s..%s)",
               vbuf, type_name(t), lo, hi);
  } else {
    diag_error(&s->diags, value_expr->span,
               "value %s is not representable in %s",
               vbuf, type_name(t));
  }
}

struct Type *query_type_of_decl(struct Sema *s, DefId def) {
  if (!s || !def_id_is_valid(def))
    return s ? s->error_type : NULL;

  struct DefInfo *di = def_info(s, def);
  if (!di) return s->error_type;

  // Primitives (i32, u8, ...) ARE types — short-circuit so we don't
  // try to const-eval their (synthetic) origin.
  if (di->kind == DECL_PRIMITIVE) {
    struct Type *t = type_for_primitive_name(s, di->name_id);
    return t ? t : s->error_type;
  }

  struct SemaDeclInfo *sdi = sema_decl_info(s, def);
  if (!sdi) return s->error_type;

  struct Span frame_span = di->span;
  SEMA_QUERY_GUARD(s, &sdi->type_query, QUERY_TYPE_OF_DECL, sdi, frame_span,
                   /*on_cached=*/sdi->type ? sdi->type : s->error_type,
                   /*on_cycle=*/s->error_type,
                   /*on_error=*/s->error_type);

  struct Type *result = s->error_type;
  struct Expr *origin = di->origin;

  if (origin && origin->kind == expr_Bind) {
    struct Expr *type_ann = origin->bind.type_ann;
    struct Expr *value    = origin->bind.value;

    if (type_ann) {
      result = resolve_type_expr(s, type_ann);
      if (value && result->kind != TY_ERROR) {
        struct ConstValue v = query_const_eval(s, value);
        check_value_fits(s, value, v, result);
      }
    } else {
      result = infer_type_from_value(s, value);
    }
  }

  sdi->type = result;
  query_slot_set_fingerprint(&sdi->type_query,
                             query_fingerprint_from_pointer(result));
  sema_query_succeed(s, &sdi->type_query);
  return result;
}
