#include "checker.h"

#include <stdio.h>

#include "../../diag/diag.h"
#include "../../parser/ast.h"
#include "../eval/const_eval.h"
#include "../modules/modules.h"  // query_module_ast
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
static bool is_struct_bind(struct Sema *s, DefId def);
static bool is_enum_bind(struct Sema *s, DefId def);

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
      if (is_struct_bind(s, def) || is_enum_bind(s, def))
        return query_type_of_def(s, def);
    }
    // Effect names / other DECL_USER shapes that aren't yet typeable.
    diag_error(&s->diags, e->span,
               "type position expects a struct, enum, or primitive type");
    return s->error_type;
  }
  case expr_SliceType: {
    // `[]T` / `[]const T` — slice of T. The parser nests `const` as
    // `unary_Const(T)`, so we peel that wrapper here to lift the
    // qualifier onto the slice's `is_const` flag (no separate
    // `const T` type — const-ness is a property of pointer-likes).
    struct Expr *inner = e->slice_type.elem;
    bool is_const = false;
    if (inner && inner->kind == expr_Unary && inner->unary.op == unary_Const) {
      is_const = true;
      inner = inner->unary.operand;
    }
    struct Type *elem = resolve_type_expr(s, inner);
    if (elem->kind == TY_ERROR) return s->error_type;
    return type_slice(s, elem, is_const);
  }
  case expr_ManyPtrType: {
    // `[^]T` / `[^]const T` — many-pointer to T. Distinct from `^T`
    // (single pointer) at the type level: many-pointers permit
    // pointer arithmetic and are what `slice.ptr` yields. Mirrors
    // Zig's `[*]T` vs `*T`.
    struct Expr *inner = e->many_ptr_type.elem;
    bool is_const = false;
    if (inner && inner->kind == expr_Unary && inner->unary.op == unary_Const) {
      is_const = true;
      inner = inner->unary.operand;
    }
    struct Type *elem = resolve_type_expr(s, inner);
    if (elem->kind == TY_ERROR) return s->error_type;
    return type_many_ptr(s, elem, is_const);
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
    // `^T` / `^const T` (single-pointer-to). The parser uses unary_Ptr
    // for this; `const` wraps the inner element via unary_Const, which
    // we peel to lift the qualifier onto the pointer's `is_const` flag.
    if (e->unary.op == unary_Ptr || e->unary.op == unary_Ref) {
      struct Expr *inner = e->unary.operand;
      bool is_const = false;
      if (inner && inner->kind == expr_Unary &&
          inner->unary.op == unary_Const) {
        is_const = true;
        inner = inner->unary.operand;
      }
      struct Type *elem = resolve_type_expr(s, inner);
      if (elem->kind == TY_ERROR) return s->error_type;
      return type_ptr(s, elem, is_const);
    }
    if (e->unary.op == unary_Const) {
      // Bare `const T` (not inside a pointer/slice). There's no
      // standalone "const T" type — const-ness is a property of
      // pointer-likes. Reject as a type expression.
      diag_error(&s->diags, e->span,
                 "'const' qualifier only applies to pointer or slice "
                 "types (e.g. `^const T`, `[]const T`)");
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

// True if `def` is a top-level Bind whose value matches `want`.
//
// Reads the AST via def_origin so the bind shape reflects the latest
// re-parse — di->origin can be a stale pointer until the def_for_name
// reuse path refreshes it, but node_to_expr is re-keyed every parse.
static bool is_bind_with_value_kind(struct Sema *s, DefId def,
                                    enum ExprKind want) {
  struct DefInfo *di = def_info(s, def);
  if (!di || di->kind != DECL_USER) return false;
  struct Expr *origin = def_origin(s, def);
  if (!origin || origin->kind != expr_Bind) return false;
  struct Expr *value = origin->bind.value;
  return value && value->kind == want;
}

// fn-shaped: top-level Bind whose RHS is a Lambda. Routes through
// query_fn_signature.
static bool is_fn_bind(struct Sema *s, DefId def) {
  return is_bind_with_value_kind(s, def, expr_Lambda);
}

// Nominal types: query_type_of_def returns TY_STRUCT(def) / TY_ENUM(def)
// without recursing into fields/variants — that's the cycle break for
// shapes like `Node :: struct { next: ^Node }`. Field/variant detail
// comes from query_struct_signature / query_enum_signature.
static bool is_struct_bind(struct Sema *s, DefId def) {
  return is_bind_with_value_kind(s, def, expr_Struct);
}

static bool is_enum_bind(struct Sema *s, DefId def) {
  return is_bind_with_value_kind(s, def, expr_Enum);
}

// True if `e` evaluates to a value the compiler knows at compile time.
//
// Three classes:
//   1. Comptime by shape — Lambda, type defs, type expressions, enum
//      literals, literals. Their existence at compile time is the
//      semantics; evaluation has no runtime component.
//   2. Comptime by composition — arithmetic, struct/array literals,
//      if/switch/block. Comptime iff all subexpressions are.
//   3. Comptime by reference — Ident resolves to a comptime-shaped
//      def: primitives, enum variants, imports, or a `::` const-bind
//      (transitively comptime by its own bound value).
//
// Always-runtime kinds:
//   - `&x` / `x^` — addresses don't exist at compile time.
//   - `arr[i]` / `arr[a..b]` — indexing/slicing produce runtime values.
//   - `obj.field` on an instance — reading from a runtime struct.
//   - Function calls — unless the fn is `comptime` (not yet wired).
//   - Var-bound idents, params, struct fields.
//   - Assignment, return, break, continue, loop, defer.
//
// Used by `type_of_value_bind` to enforce that `::` consts have a
// comptime-evaluable RHS. Mirrors the spirit of Zig's `comptime`
// analysis — though Zig is finer-grained because of its `comptime`
// parameter inference.
static bool is_comptime_evaluable(struct Sema *s, struct Expr *e);

static bool args_all_comptime(struct Sema *s, Vec *args) {
  if (!args) return true;
  for (size_t i = 0; i < args->count; i++) {
    struct Expr **slot = (struct Expr **)vec_get(args, i);
    struct Expr *arg = slot ? *slot : NULL;
    if (arg && !is_comptime_evaluable(s, arg)) return false;
  }
  return true;
}

static bool fields_all_comptime(struct Sema *s, Vec *fields) {
  if (!fields) return true;
  for (size_t i = 0; i < fields->count; i++) {
    struct ProductField *pf = (struct ProductField *)vec_get(fields, i);
    if (pf && pf->value && !is_comptime_evaluable(s, pf->value)) return false;
  }
  return true;
}

static bool is_comptime_evaluable(struct Sema *s, struct Expr *e) {
  if (!e) return false;

  switch (e->kind) {
  // ----- Comptime by shape -----
  case expr_Lit:
  case expr_Lambda:
  case expr_Struct:
  case expr_Enum:
  case expr_ArrayType:
  case expr_SliceType:
  case expr_ManyPtrType:
  case expr_EnumRef:
    return true;

  // ----- Comptime by composition -----
  case expr_Bin:
    return is_comptime_evaluable(s, e->bin.Left) &&
           is_comptime_evaluable(s, e->bin.Right);

  case expr_Unary:
    // Address-of and deref are runtime — a literal address only exists
    // once the program runs. Arithmetic / logical / type-position
    // unaries (Ptr/ManyPtr/Const) are comptime over comptime operands.
    if (e->unary.op == unary_Ref || e->unary.op == unary_Deref ||
        e->unary.op == unary_Inc || e->unary.op == unary_Dec)
      return false;
    return is_comptime_evaluable(s, e->unary.operand);

  case expr_Builtin:
    // The builtins we recognize today (@sizeOf / @alignOf / @TypeOf /
    // @typeName / @intCast) all evaluate at compile time iff their
    // arguments do.
    return args_all_comptime(s, e->builtin.args);

  case expr_Product:
    return fields_all_comptime(s, e->product.Fields);

  case expr_ArrayLit: {
    if (!e->array_lit.initializer ||
        e->array_lit.initializer->kind != expr_Product)
      return false;
    return fields_all_comptime(s, e->array_lit.initializer->product.Fields);
  }

  case expr_If:
    return is_comptime_evaluable(s, e->if_expr.condition) &&
           is_comptime_evaluable(s, e->if_expr.then_branch) &&
           (!e->if_expr.else_branch ||
            is_comptime_evaluable(s, e->if_expr.else_branch));

  case expr_Switch: {
    if (!is_comptime_evaluable(s, e->switch_expr.scrutinee)) return false;
    if (!e->switch_expr.arms) return true;
    for (size_t i = 0; i < e->switch_expr.arms->count; i++) {
      struct SwitchArm *arm =
          (struct SwitchArm *)vec_get(e->switch_expr.arms, i);
      if (arm && arm->body && !is_comptime_evaluable(s, arm->body))
        return false;
    }
    return true;
  }

  case expr_Block: {
    if (!e->block.stmts) return true;
    for (size_t i = 0; i < e->block.stmts->count; i++) {
      struct Expr **slot = (struct Expr **)vec_get(e->block.stmts, i);
      if (slot && *slot && !is_comptime_evaluable(s, *slot)) return false;
    }
    return true;
  }

  case expr_Bind:
    return e->bind.value ? is_comptime_evaluable(s, e->bind.value) : true;

  // ----- Comptime by reference (resolution-driven) -----
  case expr_Ident: {
    DefId def = query_resolve_ref(s, e, NS_VALUE);
    if (!def_id_is_valid(def))
      def = query_resolve_ref(s, e, NS_TYPE);
    if (!def_id_is_valid(def)) return false;

    struct DefInfo *di = def_info(s, def);
    if (!di) return false;

    switch (di->kind) {
    case DECL_PRIMITIVE:
    case DECL_VARIANT:
    case DECL_IMPORT:
      return true;
    case DECL_PARAM:
    case DECL_FIELD:
    case DECL_SCOPE_PARAM:
    case DECL_EFFECT_ROW:
    case DECL_LOOP_LABEL:
      return false;
    case DECL_USER: {
      // Const-bound (`::`) DECL_USER is comptime; var-bound (`:=`) is
      // runtime. Type-shaped binds (Lambda / struct / enum) are always
      // const-shaped from the parser, so they're already covered.
      struct Expr *origin = def_origin(s, def);
      if (!origin || origin->kind != expr_Bind) return false;
      return origin->bind.kind == bind_Const;
    }
    }
    return false;
  }

  case expr_Field:
    // Field access on a comptime receiver is comptime. Instance-field
    // access on a runtime receiver is runtime — covered by the
    // recursive call returning false.
    return is_comptime_evaluable(s, e->field.object);

  // ----- Always runtime -----
  case expr_Call:        // future: comptime fn marker → comptime
  case expr_Index:
  case expr_Slice:
  case expr_Assign:
  case expr_Return:
  case expr_Break:
  case expr_Continue:
  case expr_Loop:
  case expr_Defer:
  case expr_Asm:
  default:
    return false;
  }
}

// Type a non-fn Bind (DECL_USER, value is not a Lambda) — annotation
// + coercion check, or inference from the RHS.
//
// Reads origin via def_origin to pick up the latest re-parse; the
// raw di->origin is left as a debug fallback in def_origin and not
// used directly here.
static struct Type *type_of_value_bind(struct Sema *s, DefId def) {
  struct Expr *origin = def_origin(s, def);
  if (!origin || origin->kind != expr_Bind)
    return s->error_type;
  struct Expr *type_ann = origin->bind.type_ann;
  struct Expr *value    = origin->bind.value;

  // Const-bind (`::`) requires the value to be comptime-evaluable.
  // The shape-based check is conservative — it accepts everything
  // that's structurally comptime (literals, arithmetic, type defs,
  // refs to other consts, etc.) and rejects calls / runtime ops.
  // Diagnostic only; we still type-check the value below so downstream
  // errors stay coherent.
  if (origin->bind.kind == bind_Const && value &&
      !is_comptime_evaluable(s, value)) {
    diag_error(&s->diags, value->span,
               "value of '::' const binding must be comptime-evaluable");
    diag_error(&s->diags, value->span,
               "  hint: use ':=' for a runtime mutable binding");
  }

  if (type_ann) {
    struct Type *declared = resolve_type_expr(s, type_ann);
    if (declared->kind == TY_ERROR) return declared;
    // Bidirectional check: hand the value to check_expr with the
    // declared type as the expectation. check_expr handles every
    // bidirectional shape (anonymous struct literal, .Variant enum
    // literal, block-tail propagation) and falls back to a
    // synth-then-coerce path otherwise. The earlier hardcoded
    // `coerce(comptime_int, declared, ...)` was an E.1 vestige
    // that only worked for integer-literal RHSs and broke any
    // typed-bind whose value was a struct/enum/pointer/etc.
    if (value)
      check_expr(s, value, declared);
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

  // Record AST dep so any source edit invalidates this slot. The
  // value-bind path reads di->origin directly via type_of_value_bind;
  // fn/struct/enum dispatch through their signature queries which
  // record their own AST dep, but the redundancy is cheap and keeps
  // every entry path honest.
  if (scope_id_is_valid(di->owner_scope)) {
    struct ScopeInfo *si = scope_info(s, di->owner_scope);
    if (si && module_id_is_valid(si->owner_module))
      (void)query_module_ast(s, si->owner_module);
  }

  struct Type *result = s->error_type;

  switch (di->kind) {
  case DECL_USER:
    if (is_fn_bind(s, def)) {
      // Fn-shaped Bind: signature lives in its own query slot so body
      // queries that touch params don't reenter this fn-type query.
      struct FnSignature *sig = query_fn_signature(s, def);
      if (sig)
        result = type_fn(s, sig->param_types, sig->param_count, sig->ret_type);
    } else if (is_struct_bind(s, def)) {
      // Identity-only — does NOT recurse into fields, so recursive
      // struct shapes don't cycle here.
      result = type_struct(s, def);
    } else if (is_enum_bind(s, def)) {
      result = type_enum(s, def);
    } else {
      result = type_of_value_bind(s, def);
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
