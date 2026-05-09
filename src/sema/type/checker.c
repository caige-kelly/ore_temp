#include "checker.h"

#include <stdio.h>

#include "../../diag/diag.h"
#include "../../parser/ast.h"
#include "../eval/const_eval.h"
#include "../query/query_engine.h"
#include "../resolve/resolve.h"
#include "../resolve/scope_index.h"
#include "../sema.h"
#include "coerce.h"
#include "decl_data.h"
#include "decl_info.h"
#include "expr_check.h"
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

// Inferred type for a non-fn Bind RHS. Pure literal shape today;
// fn-shaped Bind RHSs route through query_fn_signature (handled in
// the DECL_USER branch of query_type_of_def, before this is called).
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

// True if `di` is a top-level Bind whose value is a Lambda. Such
// decls are fn-shaped and route through query_fn_signature.
static bool is_fn_bind(struct DefInfo *di) {
  if (!di || di->kind != DECL_USER || !di->origin) return false;
  if (di->origin->kind != expr_Bind) return false;
  struct Expr *value = di->origin->bind.value;
  return value && value->kind == expr_Lambda;
}

// Type a non-fn Bind (DECL_USER, value is not a Lambda) — annotation
// + coercion check, or inference from the RHS.
static struct Type *type_of_value_bind(struct Sema *s, struct DefInfo *di) {
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
    if (is_fn_bind(di)) {
      // Fn-shaped Bind: signature lives in its own query slot so body
      // queries that touch params don't reenter this fn-type query.
      struct FnSignature *sig = query_fn_signature(s, def);
      if (sig)
        result = type_fn(s, sig->param_types, sig->param_count, sig->ret_type);
    } else {
      result = type_of_value_bind(s, di);
    }
    break;
  case DECL_PARAM: {
    // ParamLocator gives us (parent_fn, index); the actual type lives
    // in the parent fn's FnSignature. Querying the signature records
    // the dep on its producer slot — when the parent fn re-walks, this
    // param's slot invalidates correctly.
    struct ParamLocator *loc = param_locator_get(s, def);
    if (loc && def_id_is_valid(loc->parent_fn)) {
      struct FnSignature *sig = query_fn_signature(s, loc->parent_fn);
      if (sig && loc->index < sig->param_count)
        result = sig->param_types[loc->index];
    }
    break;
  }
  case DECL_FIELD:
    // Struct field types arrive when struct member resolution lands
    // (E.3) — at that point a FieldData side table mirrors ParamData.
    // For now, fields don't yet have data; types fall through to error.
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
