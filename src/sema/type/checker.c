#include "checker.h"

#include <stdio.h>

#include "../../common/vec.h"
#include "../../diag/diag.h"
#include "../../parser/ast.h"
#include "../eval/const_eval.h"
#include "../modules/def_map.h" // query_top_level_index, TopLevelEntry
#include "../modules/modules.h" // query_module_ast
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
  if (!e)
    return s->error_type;

  switch (e->kind) {
  case expr_Ident: {
    DefId def = query_resolve_ref(s, e, NS_TYPE);
    if (!def_id_is_valid(def))
      return s->error_type; // resolve already emitted "name not found"
    struct DefInfo *di = def_info(s, def);
    if (!di)
      return s->error_type;
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
    if (elem->kind == TY_ERROR)
      return s->error_type;
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
    if (elem->kind == TY_ERROR)
      return s->error_type;
    return type_many_ptr(s, elem, is_const);
  }
  case expr_ArrayType: {
    // `[N]T` — N-element array of T. Size is const-evaluated.
    struct Type *elem = resolve_type_expr(s, e->array_type.elem);
    if (elem->kind == TY_ERROR)
      return s->error_type;
    struct ConstValue size_val = query_const_eval(s, e->array_type.size);
    if (size_val.kind != CONST_INT || size_val.int_val < 0) {
      diag_error(&s->diags, e->array_type.size->span,
                 "array size must be a non-negative comptime integer");
      return s->error_type;
    }
    return type_array(s, elem, (uint64_t)size_val.int_val);
  }
  case expr_FnType: {
    // `Fn(T1, T2, ...) -> R` — type-position-only fn constructor. The
    // parser already split this from value-position `fn(...) { body }`
    // (which produces expr_Lambda), so this case has no name vs. type
    // ambiguity to resolve: each param entry is a type expression we
    // recurse into directly. Pre-Fn-split, the equivalent code lived
    // here for expr_Lambda and had to fall back to a primitive-name
    // heuristic when params were anonymous (B17). The split deletes
    // that heuristic class entirely.
    Vec *pts = e->fn_type.param_types;
    size_t n = pts ? pts->count : 0;
    struct Type **param_types = NULL;
    if (n > 0) {
      param_types = arena_alloc(&s->arena, sizeof(struct Type *) * n);
      for (size_t i = 0; i < n; i++) {
        struct Expr **slot = (struct Expr **)vec_get(pts, i);
        struct Expr *ty_expr = slot ? *slot : NULL;
        if (!ty_expr) {
          diag_error(&s->diags, e->span,
                     "function type parameter #%zu has no type", i);
          return s->error_type;
        }
        param_types[i] = resolve_type_expr(s, ty_expr);
        if (param_types[i]->kind == TY_ERROR)
          return s->error_type;
      }
    }
    struct Type *ret_type = e->fn_type.ret_type
                                ? resolve_type_expr(s, e->fn_type.ret_type)
                                : s->void_type;
    if (ret_type->kind == TY_ERROR)
      return s->error_type;
    return type_fn(s, param_types, n, ret_type);
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
      if (elem->kind == TY_ERROR)
        return s->error_type;
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
    if (e->unary.op == unary_Optional) {
      // `?T` — value-or-nil. The interner collapses `?(?T)` to `?T`,
      // so nesting at the source level (e.g. `??i32`) is harmless.
      struct Type *elem = resolve_type_expr(s, e->unary.operand);
      if (elem->kind == TY_ERROR)
        return s->error_type;
      return type_optional(s, elem);
    }
    diag_error(&s->diags, e->span,
               "unary operator is not a valid type expression");
    return s->error_type;
  }
  case expr_Lambda:
    // Lowercase `fn(...)` in type position — common mistake. After
    // the Fn-split (PR 3 cleanup), `fn` is value-only; `Fn` is the
    // type-position spelling. Tell the user precisely.
    diag_error(&s->diags, e->span,
               "function types use capital `Fn(...)`; lowercase `fn` "
               "is the value/definition keyword");
    return s->error_type;
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
  if (!value)
    return s->error_type;
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
  if (!di || di->kind != DECL_USER)
    return false;
  struct Expr *origin = def_origin(s, def);
  if (!origin || origin->kind != expr_Bind)
    return false;
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

// "Is `e` comptime-evaluable?" — used by `type_of_value_bind` to
// enforce that `::` consts have a comptime-evaluable RHS. Now a
// real query (see src/sema/eval/const_eval.c). The dep graph
// captures editing transitively-referenced const-binds, so a future
// edit to a chain root invalidates downstream comptime-checks.
//
// Three semantic classes (preserved from the prior walker):
//   1. Comptime by shape — Lambda, type defs, type expressions,
//      literals. No runtime component.
//   2. Comptime by composition — arithmetic, struct/array literals,
//      if/switch/block. Comptime iff all subexpressions are.
//   3. Comptime by reference — Ident resolves to a comptime-shaped
//      def: primitives, enum variants, imports, or a `::` const-bind
//      (transitively comptime by its own bound value).

// Type a non-fn Bind (DECL_USER, value is not a Lambda) — annotation
// + coercion check, or inference from the RHS.
//
// Reads origin via def_origin so we always see the current revision's
// AST: def_origin routes top-level lookups through the module's
// top-level index (name-keyed), not through a cached Expr pointer.
static struct Type *type_of_value_bind(struct Sema *s, DefId def) {
  struct Expr *origin = def_origin(s, def);
  if (!origin || origin->kind != expr_Bind)
    return s->error_type;
  struct Expr *type_ann = origin->bind.type_ann;
  struct Expr *value = origin->bind.value;

  // Const-bind (`::`) requires the value to be comptime-evaluable.
  // The shape-based check is conservative — it accepts everything
  // that's structurally comptime (literals, arithmetic, type defs,
  // refs to other consts, etc.) and rejects calls / runtime ops.
  // Diagnostic only; we still type-check the value below so downstream
  // errors stay coherent.
  if (origin->bind.kind == bind_Const && value &&
      !query_is_comptime(s, value)) {
    diag_error(&s->diags, value->span,
               "value of '::' const binding must be comptime-evaluable");
    diag_error(&s->diags, value->span,
               "  hint: use ':=' for a runtime mutable binding");
  }

  if (type_ann) {
    struct Type *declared = resolve_type_expr(s, type_ann);
    if (declared->kind == TY_ERROR)
      return declared;
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
  if (!di)
    return s->error_type;

  // Primitives short-circuit: their Type* is intrinsic, no slot
  // machinery needed.
  if (di->kind == DECL_PRIMITIVE) {
    struct Type *t = type_for_primitive_name(s, di->name_id);
    return t ? t : s->error_type;
  }

  struct SemaDeclInfo *sdi = sema_decl_info(s, def);
  if (!sdi)
    return s->error_type;

  struct Span frame_span = def_span(s, def);
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
      struct StructSignature *sig =
          query_struct_signature(s, loc->parent_struct);
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

// Driver-level typecheck pass. Forces every top-level decl's type
// query and (for binds) its value's expr-type query so diagnostics
// flow into Sema.diags before any dumpers run.
//
// Mirrors the iteration in dump_tyck (src/sema/type/dump.c), minus
// the printing — both want to fully type the module, but only the
// dumper renders the result. Pre-PR-3-Layer-0, the production
// driver only ran def_map + scope_index, so a typed-bind range
// overflow like `let x: u8 = 1024` silently compiled.
void sema_check_module(struct Sema *s, ModuleId mid) {
  if (!s)
    return;
  Vec *idx = query_top_level_index(s, mid);
  if (!idx)
    return;
  for (size_t i = 0; i < idx->count; i++) {
    struct TopLevelEntry *e = (struct TopLevelEntry *)vec_get(idx, i);
    if (!e || !e->node || e->node->kind != expr_Bind)
      continue;
    DefId def = query_def_for_name(s, mid, e->name_id);
    if (!def_id_is_valid(def))
      continue;
    (void)query_type_of_def(s, def);
    // Walking the value forces the body's expression types — this is
    // where coerce range-checks against typed binds (`x : u8 = MAX`)
    // and check_expr's bidirectional path actually fire.
    struct Expr *value = e->node->bind.value;
    if (value)
      (void)query_type_of_expr(s, value);
  }
}
