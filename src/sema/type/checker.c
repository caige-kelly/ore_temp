#include "checker.h"

#include <stdio.h>

#include "../../diag/diag.h"
#include "../../parser/ast.h"
#include "../eval/const_eval.h"
#include "../query/query_engine.h"
#include "../resolve/resolve.h"
#include "../sema.h"
#include "coerce.h"
#include "decl_info.h"
#include "type.h"

// Resolve a type-position Expr* to a Type*. Public — used by both
// query_type_of_def's annotation path and query_type_of_expr's
// type-position children (return types, param annotations,
// `[N]T` shapes nested in expressions).
struct Type *resolve_type_expr(struct Sema *s, struct Expr *e) {
  if (!e) return s->error_type;

  switch (e->kind) {
  case expr_Ident: {
    DefId def = query_resolve_ref(s, e, NS_TYPE);
    if (!def_id_is_valid(def))
      return s->error_type;  // resolve already emitted "name not found"
    struct DefInfo *di = def_info(s, def);
    if (!di) return s->error_type;
    if (di->kind == DECL_PRIMITIVE) {
      struct Type *t = type_for_primitive_name(s, di->name_id);
      return t ? t : s->error_type;
    }
    // User-defined types (struct/enum/effect names) land in E.3.
    diag_error(&s->diags, e->span,
               "type position expects a primitive type (E.2 limit; "
               "user-defined types arrive in E.3)");
    return s->error_type;
  }
  case expr_SliceType: {
    // `[]T` — slice of T. Const-qualifier handling waits for `unary_Const`
    // wrapper recognition (E.3+).
    struct Type *elem = resolve_type_expr(s, e->slice_type.elem);
    if (elem->kind == TY_ERROR) return s->error_type;
    return type_slice(s, elem, /*is_const=*/false);
  }
  case expr_ManyPtrType: {
    // `[^]T` — many-pointer to T.
    struct Type *elem = resolve_type_expr(s, e->many_ptr_type.elem);
    if (elem->kind == TY_ERROR) return s->error_type;
    return type_ptr(s, elem, /*is_const=*/false);
  }
  case expr_ArrayType: {
    // `[N]T` — N-element array of T. Size is const-evaluated.
    struct Type *elem = resolve_type_expr(s, e->array_type.elem);
    if (elem->kind == TY_ERROR) return s->error_type;
    struct ConstValue size_val = query_const_eval(s, e->array_type.size);
    if (size_val.kind != CONST_INT || size_val.int_val < 0) {
      diag_error(&s->diags, e->array_type.size->span,
                 "array size must be a non-negative comptime integer");
      return s->error_type;
    }
    return type_array(s, elem, (uint64_t)size_val.int_val);
  }
  case expr_Unary: {
    // `^T` (single-pointer-to). The parser uses unary_Ptr for this.
    if (e->unary.op == unary_Ptr || e->unary.op == unary_Ref) {
      struct Type *elem = resolve_type_expr(s, e->unary.operand);
      if (elem->kind == TY_ERROR) return s->error_type;
      return type_ptr(s, elem, /*is_const=*/false);
    }
    if (e->unary.op == unary_Const) {
      // `const T` — qualifier on the inner type. Today's pointer/slice
      // structs carry an `is_const` field but we don't have a generic
      // "const T" Type. For non-pointer types, a const-qualifier is
      // just a binding-level modifier, not a Type. Defer.
      diag_error(&s->diags, e->span,
                 "const qualifiers in type position are not yet supported");
      return s->error_type;
    }
    diag_error(&s->diags, e->span,
               "unary operator is not a valid type expression");
    return s->error_type;
  }
  default:
    diag_error(&s->diags, e->span,
               "type expressions of this shape are not yet supported");
    return s->error_type;
  }
}

// Inferred type for a Bind without annotation. Today: pure literal
// shape — int / float / nothing-known. Bigger Bind shapes (Lambda,
// Struct, Enum) become real types when query_type_of_expr handles
// them — see expr_check.c.
static struct Type *infer_type_from_value(struct Sema *s, struct Expr *value) {
  if (!value) return s->error_type;

  // For Lambda values, defer to query_type_of_expr — it builds the
  // TY_FN and validates the body. Avoids duplicating the logic here.
  // (Forward declaration to break the include cycle with expr_check.h.)
  extern struct Type *query_type_of_expr(struct Sema *s, struct Expr *e);
  if (value->kind == expr_Lambda)
    return query_type_of_expr(s, value);

  // Literals: comptime_int / comptime_float / nothing-known.
  struct ConstValue v = query_const_eval(s, value);
  switch (v.kind) {
  case CONST_INT:   return s->comptime_int_type;
  case CONST_FLOAT: return s->comptime_float_type;
  case CONST_NONE:  return s->error_type;
  }
  return s->error_type;
}

// Type a top-level Bind (DECL_USER) — annotation + coercion check, or
// inference from the RHS.
static struct Type *type_of_user_bind(struct Sema *s, struct DefInfo *di) {
  struct Expr *origin = di->origin;
  if (!origin || origin->kind != expr_Bind)
    return s->error_type;
  struct Expr *type_ann = origin->bind.type_ann;
  struct Expr *value    = origin->bind.value;

  if (type_ann) {
    struct Type *declared = resolve_type_expr(s, type_ann);
    if (declared->kind == TY_ERROR) return declared;
    if (value) {
      struct ConstValue v = query_const_eval(s, value);
      // For Lambda RHSs the value isn't const-evaluable but its type
      // still flows through coerce via query_type_of_expr. Here in
      // E.2 we only handle the const-numeric case explicitly; deeper
      // value→annotation checks (e.g., a Lambda type matching the
      // declared fn type) land when expr_check is wired in.
      coerce(s, s->comptime_int_type, declared, v, value->span);
    }
    return declared;
  }

  return infer_type_from_value(s, value);
}

struct Type *query_type_of_def(struct Sema *s, DefId def) {
  if (!s || !def_id_is_valid(def))
    return s ? s->error_type : NULL;

  struct DefInfo *di = def_info(s, def);
  if (!di) return s->error_type;

  // Primitives short-circuit: their Type* is intrinsic, no slot
  // machinery needed.
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

  switch (di->kind) {
  case DECL_USER:
    result = type_of_user_bind(s, di);
    break;
  case DECL_PARAM:
    // Resolve from the stored annotation expression. If unannotated
    // (currently impossible — the parser requires `name : type`),
    // fall through to error.
    if (di->type_ann_expr)
      result = resolve_type_expr(s, di->type_ann_expr);
    break;
  case DECL_FIELD:
    // Struct field types arrive when struct member resolution lands
    // (E.3). For now: stored annotation if present, else error.
    if (di->type_ann_expr)
      result = resolve_type_expr(s, di->type_ann_expr);
    break;
  case DECL_IMPORT:
    // Module values get a placeholder "module" type. Real module-
    // type semantics (with member resolution etc.) are E.3+.
    result = s->module_type ? s->module_type : s->error_type;
    break;
  case DECL_SCOPE_PARAM:
  case DECL_EFFECT_ROW:
  case DECL_LOOP_LABEL:
  case DECL_PRIMITIVE:
    // Already handled above (PRIMITIVE) or have no value-type
    // semantics (the rest are syntactic positions, not values).
    result = s->error_type;
    break;
  }

  sdi->type = result;
  query_slot_set_fingerprint(&sdi->type_query,
                             query_fingerprint_from_pointer(result));
  sema_query_succeed(s, &sdi->type_query);
  return result;
}
