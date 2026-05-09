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

// Forward declarations — predicates used by resolve_type_expr that
// are defined alongside their natural query siblings further down.
static bool is_struct_bind(struct DefInfo *di);
static bool is_enum_bind(struct DefInfo *di);

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
    if (di->kind == DECL_USER) {
      // User-defined nominal types (struct / enum). Defer to
      // query_type_of_def, which produces TY_STRUCT(def) /
      // TY_ENUM(def) without recursing into fields/variants — that's
      // the cycle break for self-referential shapes like `Node ::
      // struct { next: ^Node }`.
      if (is_struct_bind(di) || is_enum_bind(di))
        return query_type_of_def(s, def);
    }
    // Effect names / other DECL_USER shapes that aren't yet typeable.
    diag_error(&s->diags, e->span,
               "type position expects a struct, enum, or primitive type");
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

// Inferred type for a non-fn Bind RHS. Defers to query_type_of_expr —
// the per-Expr type is the canonical source of truth, with literal
// shapes, struct/enum constructors, and any other expression shape
// flowing through the same machinery. Fn-shaped Bind RHSs route
// through query_fn_signature in the DECL_USER branch above and never
// reach this helper.
static struct Type *infer_type_from_value(struct Sema *s, struct Expr *value) {
  if (!value) return s->error_type;
  struct Type *t = query_type_of_expr(s, value);
  return t ? t : s->error_type;
}

// True if `di` is a top-level Bind whose value is a Lambda. Such
// decls are fn-shaped and route through query_fn_signature.
static bool is_fn_bind(struct DefInfo *di) {
  if (!di || di->kind != DECL_USER || !di->origin) return false;
  if (di->origin->kind != expr_Bind) return false;
  struct Expr *value = di->origin->bind.value;
  return value && value->kind == expr_Lambda;
}

// True if `di` is a top-level Bind whose value is a struct decl.
// These are nominal types — query_type_of_def returns TY_STRUCT(def)
// without recursing into fields (cycle break for `Node :: struct
// { next: ^Node }`). Field detail comes from query_struct_signature.
static bool is_struct_bind(struct DefInfo *di) {
  if (!di || di->kind != DECL_USER || !di->origin) return false;
  if (di->origin->kind != expr_Bind) return false;
  struct Expr *value = di->origin->bind.value;
  return value && value->kind == expr_Struct;
}

static bool is_enum_bind(struct DefInfo *di) {
  if (!di || di->kind != DECL_USER || !di->origin) return false;
  if (di->origin->kind != expr_Bind) return false;
  struct Expr *value = di->origin->bind.value;
  return value && value->kind == expr_Enum;
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
    } else if (is_struct_bind(di)) {
      // Identity-only — does NOT recurse into fields, so recursive
      // struct shapes don't cycle here.
      result = type_struct(s, def);
    } else if (is_enum_bind(di)) {
      result = type_enum(s, def);
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
  case DECL_FIELD: {
    // FieldLocator → parent struct's signature → fields[index].type.
    // Mirrors the param path; the struct signature is the producer.
    struct FieldLocator *loc = field_locator_get(s, def);
    if (loc && def_id_is_valid(loc->parent_struct)) {
      struct StructSignature *sig = query_struct_signature(s, loc->parent_struct);
      if (sig && loc->index < sig->field_count)
        result = sig->fields[loc->index].type;
    }
    break;
  }
  case DECL_VARIANT: {
    // A variant's "type" is its parent enum (a value of `Color.Red`
    // has type `Color`). Calling query_enum_signature records the dep
    // on the producer; we ignore the per-variant int value here since
    // it doesn't affect the typechecker (it's codegen / pattern-match
    // territory).
    struct VariantLocator *loc = variant_locator_get(s, def);
    if (loc && def_id_is_valid(loc->parent_enum)) {
      (void)query_enum_signature(s, loc->parent_enum);
      result = type_enum(s, loc->parent_enum);
    }
    break;
  }
  case DECL_IMPORT:
    // Module values get a placeholder "module" type. Real module-
    // type semantics (with member resolution etc.) arrive when
    // multi-file @import lands.
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
