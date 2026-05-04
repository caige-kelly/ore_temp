#include "const_eval.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "../compiler/compiler.h"
#include "../parser/ast.h"
#include "checker.h"
#include "decls.h"
#include "instantiate.h"
#include "layout.h"
#include "sema.h"
#include "sema_internal.h"
#include "target.h"
#include "type.h"

// Comptime evaluation limits. These are upper bounds on how much work the
// interpreter is willing to do for a single fold attempt; if they're hit we
// bail with EVAL_ERROR so a buggy comptime program can't lock up the build.
//
// Values are conservative defaults — fine for hand-written code, generous
// enough that real metaprograms (build.ore, generic specialization) won't
// trip them. Promote to a `struct ConstEvalLimits` on Sema if/when we need
// per-build tuning (`--comptime-loop-fuel=N` etc.).
#define ORE_COMPTIME_CALL_DEPTH_MAX 100
#define ORE_COMPTIME_LOOP_FUEL 1000000

struct ConstValue sema_decl_value(struct Sema *s, struct Decl *d) {
  if (!d)
    return sema_const_invalid();
  struct SemaDeclInfo *info = sema_decl_info(s, d);
  return info ? info->value : sema_const_invalid();
}

// ----- ConstValue constructors / utilities -----

struct ConstValue sema_const_invalid(void) {
  struct ConstValue v = {0};
  v.kind = CONST_INVALID;
  return v;
}

struct ConstValue sema_const_int(int64_t value) {
  struct ConstValue v = {0};
  v.kind = CONST_INT;
  v.int_val = value;
  return v;
}

struct ConstValue sema_const_float(double value) {
  struct ConstValue v = {0};
  v.kind = CONST_FLOAT;
  v.float_val = value;
  return v;
}

struct ConstValue sema_const_bool(bool value) {
  struct ConstValue v = {0};
  v.kind = CONST_BOOL;
  v.bool_val = value;
  return v;
}

struct ConstValue sema_const_type(struct Type *type) {
  struct ConstValue v = {0};
  v.kind = CONST_TYPE;
  v.type_val = type;
  return v;
}

struct ConstValue sema_const_string(uint32_t string_id) {
  struct ConstValue v = {0};
  v.kind = CONST_STRING;
  v.string_id = string_id;
  return v;
}

struct ConstValue sema_const_void(void) {
  struct ConstValue v = {0};
  v.kind = CONST_VOID;
  return v;
}

struct ConstValue sema_const_struct(struct ConstStruct *sv) {
  struct ConstValue v = {0};
  v.kind = CONST_STRUCT;
  v.struct_val = sv;
  return v;
}

struct ConstValue sema_const_function(struct Decl *decl) {
  struct ConstValue v = {0};
  v.kind = CONST_FUNCTION;
  v.fn_decl = decl;
  return v;
}

struct ConstValue sema_const_array(struct ConstArray *av) {
  struct ConstValue v = {0};
  v.kind = CONST_ARRAY;
  v.array_val = av;
  return v;
}

void sema_print_const_value(struct ConstValue v, struct Sema *s) {
  switch (v.kind) {
  case CONST_INT:
    printf("%lld", (long long)v.int_val);
    break;
  case CONST_FLOAT:
    printf("%g", v.float_val);
    break;
  case CONST_BOOL:
    printf("%s", v.bool_val ? "true" : "false");
    break;
  case CONST_STRING: {
    const char *str = (s && s->pool) ? pool_get(s->pool, v.string_id, 0) : NULL;
    printf("\"%s\"", str ? str : "?");
    break;
  }
  case CONST_TYPE: {
    char buf[128];
    printf("type(%s)", sema_type_display_name(s, v.type_val, buf, sizeof(buf)));
    break;
  }
  case CONST_FUNCTION: {
    const char *name = (v.fn_decl && s && s->pool)
                           ? pool_get(s->pool, v.fn_decl->name.string_id, 0)
                           : NULL;
    printf("fn(%s)", name ? name : "?");
    break;
  }
  case CONST_STRUCT:
    printf("struct{...}");
    break;
  case CONST_ARRAY: {
    size_t n = (v.array_val && v.array_val->elements)
                   ? v.array_val->elements->count
                   : 0;
    printf("array[%zu]", n);
    break;
  }
  case CONST_VOID:
    printf("()");
    break;
  case CONST_INVALID:
    printf("?");
    break;
  }
}

bool sema_const_value_is_valid(struct ConstValue value) {
  return value.kind != CONST_INVALID;
}

static bool call_cache_lookup(struct Sema *s, struct Decl *fn_decl,
                              struct ComptimeArgTuple *arg_values,
                              struct ConstValue *out) {
  Vec *bucket =
      (Vec *)hashmap_get(&s->call_cache, (uint64_t)(uintptr_t)fn_decl);
  if (!bucket)
    return false;
  for (size_t i = 0; i < bucket->count; i++) {
    struct ComptimeCallCacheEntry **ep =
        (struct ComptimeCallCacheEntry **)vec_get(bucket, i);
    struct ComptimeCallCacheEntry *entry = ep ? *ep : NULL;
    if (!entry)
      continue;
    // Single canonical equality predicate, shared with instantiate.c.
    if (sema_arg_tuple_equal(entry->args, arg_values)) {
      if (out)
        *out = entry->result;
      return true;
    }
  }
  return false;
}

static void call_cache_store(struct Sema *s, struct Decl *fn_decl,
                             struct ComptimeArgTuple *arg_values,
                             struct ConstValue result) {
  Vec *bucket =
      (Vec *)hashmap_get(&s->call_cache, (uint64_t)(uintptr_t)fn_decl);
  if (!bucket) {
    bucket = vec_new_in(s->arena, sizeof(struct ComptimeCallCacheEntry *));
    hashmap_put(&s->call_cache, (uint64_t)(uintptr_t)fn_decl, bucket);
  }
  struct ComptimeCallCacheEntry *entry =
      arena_alloc(s->arena, sizeof(struct ComptimeCallCacheEntry));
  entry->args = arg_values; // shallow ref — caller's Vec is arena-owned
  entry->result = result;
  vec_push(bucket, &entry);
}

bool sema_const_value_equal(struct ConstValue a, struct ConstValue b) {
  if (a.kind != b.kind)
    return false;
  switch (a.kind) {
  case CONST_INT:
    return a.int_val == b.int_val;
  case CONST_FLOAT:
    return a.float_val == b.float_val;
  case CONST_BOOL:
    return a.bool_val == b.bool_val;
  case CONST_TYPE:
    return a.type_val == b.type_val;
  case CONST_STRING:
    return a.string_id == b.string_id;
  case CONST_FUNCTION:
    return a.fn_decl == b.fn_decl;
  case CONST_STRUCT: {
    struct ConstStruct *a_sv = a.struct_val;
    struct ConstStruct *b_sv = b.struct_val;
    if (!a_sv || !b_sv)
      return a_sv == b_sv;
    if (a_sv->type != b_sv->type)
      return false;
    if (!a_sv->fields || !b_sv->fields)
      return a_sv->fields == b_sv->fields;
    if (a_sv->fields->count != b_sv->fields->count)
      return false;
    for (size_t i = 0; i < a_sv->fields->count; i++) {
      struct ConstStructField *af =
          (struct ConstStructField *)vec_get(a_sv->fields, i);
      struct ConstStructField *bf =
          (struct ConstStructField *)vec_get(b_sv->fields, i);
      if (!af || !bf)
        return false;
      if (af->name_id != bf->name_id)
        return false;
      if (!sema_const_value_equal(af->value, bf->value))
        return false;
    }
    return true;
  }
  case CONST_ARRAY: {
    struct ConstArray *xa = a.array_val;
    struct ConstArray *xb = b.array_val;
    if (!xa || !xb)
      return xa == xb;
    if (xa->elem_type != xb->elem_type)
      return false;
    if (!xa->elements || !xb->elements)
      return xa->elements == xb->elements;
    if (xa->elements->count != xb->elements->count)
      return false;
    for (size_t i = 0; i < xa->elements->count; i++) {
      struct ConstValue *av = (struct ConstValue *)vec_get(xa->elements, i);
      struct ConstValue *bv = (struct ConstValue *)vec_get(xb->elements, i);
      if (!av || !bv)
        return false;
      if (!sema_const_value_equal(*av, *bv))
        return false;
    }
    return true;
  }
  case CONST_VOID:
    return true;
  case CONST_INVALID:
    return true;
  }
  return false;
}

// ----- EvalResult -----

struct EvalResult sema_eval_normal(struct ConstValue v) {
  return (struct EvalResult){.control = EVAL_NORMAL, .value = v};
}

struct EvalResult sema_eval_err(void) {
  return (struct EvalResult){.control = EVAL_ERROR,
                             .value = sema_const_invalid()};
}

// ----- ComptimeEnv -----

struct ComptimeEnv *sema_comptime_env_new(struct Sema *s,
                                          struct ComptimeEnv *parent) {
  if (!s || !s->arena)
    return NULL;
  struct ComptimeEnv *env = arena_alloc(s->arena, sizeof(struct ComptimeEnv));
  if (!env)
    return NULL;
  env->bindings = vec_new_in(s->arena, sizeof(struct ComptimeBinding));
  env->parent = parent;
  return env;
}

void sema_comptime_env_bind(struct Sema *s, struct ComptimeEnv *env,
                            struct Decl *decl, struct ConstValue value) {
  if (!env || !decl)
    return;
  struct ComptimeCell *cell =
      arena_alloc(s->arena, sizeof(struct ComptimeCell));
  cell->value = value;
  struct ComptimeBinding b = {.decl = decl, .cell = cell};
  vec_push(env->bindings, &b);
}

bool sema_comptime_env_lookup(struct ComptimeEnv *env, struct Decl *decl,
                              struct ConstValue *out) {
  for (struct ComptimeEnv *cur = env; cur; cur = cur->parent) {
    if (!cur->bindings)
      continue;
    for (size_t i = cur->bindings->count; i > 0; i--) {
      struct ComptimeBinding *b =
          (struct ComptimeBinding *)vec_get(cur->bindings, i - 1);
      if (b && b->decl == decl) {
        if (out)
          *out = b->cell->value;
        return true;
      }
    }
  }
  return false;
}

void sema_comptime_env_assign(struct Sema *s, struct ComptimeEnv *env,
                              struct Decl *decl, struct ConstValue value) {
  for (struct ComptimeEnv *cur = env; cur; cur = cur->parent) {
    if (!cur->bindings)
      continue;
    for (size_t i = cur->bindings->count; i > 0; i--) {
      struct ComptimeBinding *b =
          (struct ComptimeBinding *)vec_get(cur->bindings, i - 1);
      if (b && b->decl == decl) {
        b->cell->value = value;
        return;
      }
    }
  }
}

// ----- helpers -----

static struct ConstValue eval_int_literal(struct Sema *s, struct Expr *expr) {
  const char *text = s->pool ? pool_get(s->pool, expr->lit.string_id, 0) : NULL;
  if (!text || !*text)
    return sema_const_invalid();

  // Copy text, stripping digit-separators ('_').
  char buf[64];
  size_t out = 0;
  for (size_t in = 0; text[in] && out < sizeof(buf) - 1; in++) {
    if (text[in] != '_')
      buf[out++] = text[in];
  }
  buf[out] = '\0';
  if (out == 0)
    return sema_const_invalid();

  errno = 0;
  char *end = NULL;
  long long value = strtoll(buf, &end, 0);
  if (errno != 0 || (end && *end != '\0'))
    return sema_const_invalid();
  return sema_const_int((int64_t)value);
}

static struct ConstValue eval_float_literal(struct Sema *s, struct Expr *expr) {
  const char *text = s->pool ? pool_get(s->pool, expr->lit.string_id, 0) : NULL;
  if (!text || !*text)
    return sema_const_invalid();

  errno = 0;
  char *end = NULL;
  double value = strtod(text, &end);
  if (errno != 0 || (end && *end != '\0'))
    return sema_const_invalid();
  return sema_const_float(value);
}

static struct EvalResult eval_lit(struct Sema *s, struct Expr *expr) {
  switch (expr->lit.kind) {
  case lit_Int:
    return sema_eval_normal(eval_int_literal(s, expr));
  case lit_Float:
    return sema_eval_normal(eval_float_literal(s, expr));
  case lit_True:
    return sema_eval_normal(sema_const_bool(true));
  case lit_False:
    return sema_eval_normal(sema_const_bool(false));
  case lit_String:
    return sema_eval_normal(sema_const_string(expr->lit.string_id));
  case lit_Byte:
    return sema_eval_normal(eval_int_literal(s, expr));
  case lit_Nil:
  default:
    return sema_eval_normal(sema_const_invalid());
  }
}

static bool is_target_field_chain(struct Sema *s, struct Expr *expr,
                                  const char **out_field) {
  if (!expr || expr->kind != expr_Field)
    return false;
  struct Expr *obj = expr->field.object;
  if (!obj || obj->kind != expr_Builtin)
    return false;
  if (obj->builtin.name_id != s->name_target)
    return false;
  if (!s->pool)
    return false;
  const char *field = pool_get(s->pool, expr->field.field.string_id, 0);
  if (!field)
    return false;
  if (out_field)
    *out_field = field;
  return true;
}

static struct TargetInfo target_for(struct Sema *s) {
  if (s && s->compiler)
    return s->compiler->target;
  return target_default_host();
}

static struct EvalResult eval_target_field(struct Sema *s, const char *field) {
  struct TargetInfo t = target_for(s);
  uint32_t id = 0;
  if (strcmp(field, "os") == 0) {
    const char *name = target_os_name(t.os);
    id = pool_intern(s->pool, name, strlen(name));
    return sema_eval_normal(sema_const_string(id));
  }
  if (strcmp(field, "arch") == 0) {
    const char *name = target_arch_name(t.arch);
    id = pool_intern(s->pool, name, strlen(name));
    return sema_eval_normal(sema_const_string(id));
  }
  if (strcmp(field, "pointer_size") == 0) {
    return sema_eval_normal(sema_const_int((int64_t)t.pointer_size));
  }
  return sema_eval_normal(sema_const_invalid());
}

static struct EvalResult eval_builtin(struct Sema *s, struct Expr *expr,
                                      struct ComptimeEnv *env) {
  uint32_t bn = expr->builtin.name_id;
  if (bn == s->name_sizeOf || bn == s->name_alignOf) {
    bool is_size = (bn == s->name_sizeOf);
    if (!expr->builtin.args || expr->builtin.args->count == 0)
      return sema_eval_normal(sema_const_invalid());

    struct Expr **arg_p = (struct Expr **)vec_get(expr->builtin.args, 0);
    struct Expr *arg = arg_p ? *arg_p : NULL;
    if (!arg)
      return sema_eval_normal(sema_const_invalid());

    struct Type *arg_type = NULL;
    if (arg->kind == expr_Ident) {
      struct EvalResult cv = sema_const_eval_expr(s, arg, env);
      if (cv.control != EVAL_NORMAL)
        return cv;
      if (cv.value.kind == CONST_TYPE)
        arg_type = cv.value.type_val;
    }
    if (!arg_type)
      arg_type = sema_infer_type_expr(s, arg);
    if (!arg_type || sema_type_is_errorish(arg_type))
      return sema_eval_normal(sema_const_invalid());

    struct TypeLayout layout = sema_layout_of_type_at(s, arg_type, expr->span);
    if (!layout.complete)
      return sema_eval_normal(sema_const_invalid());

    return sema_eval_normal(
        sema_const_int((int64_t)(is_size ? layout.size : layout.align)));
  }

  if (bn == s->name_target) {
    // bare @target is not a value — it's only meaningful via field access.
    return sema_eval_normal(sema_const_invalid());
  }

  if (bn == s->name_returnType) {
    // `@returnType(EXPR)` evaluates EXPR's type and returns its return-
    // type slot as a CONST_TYPE. EXPR is typically a function parameter
    // or identifier referencing a function value.
    if (!expr->builtin.args || expr->builtin.args->count == 0) {
      sema_error(s, expr->span, "@returnType requires one function argument");
      return sema_eval_err();
    }
    struct Expr **arg_p = (struct Expr **)vec_get(expr->builtin.args, 0);
    struct Expr *arg = arg_p ? *arg_p : NULL;
    if (!arg)
      return sema_eval_normal(sema_const_invalid());

    struct Type *fn_type = sema_infer_expr(s, arg);
    if (!fn_type || sema_type_is_errorish(fn_type)) {
      return sema_eval_normal(sema_const_invalid());
    }
    if (fn_type->kind != TYPE_FUNCTION) {
      char nm[128];
      sema_error(s, arg->span, "@returnType expects a function, found %s",
                 sema_type_display_name(s, fn_type, nm, sizeof(nm)));
      return sema_eval_err();
    }
    return sema_eval_normal(
        sema_const_type(fn_type->ret ? fn_type->ret : s->void_type));
  }

  // Unknown @-builtin. Diagnose explicitly so the user gets an actionable
  // message instead of a downstream "couldn't be folded" cascade.
  const char *name =
      s->pool ? pool_get(s->pool, expr->builtin.name_id, 0) : NULL;
  sema_error(s, expr->span, "unknown comptime builtin '@%s'",
             name ? name : "?");
  return sema_eval_err();
}

static struct EvalResult eval_ident(struct Sema *s, struct Expr *expr,
                                    struct ComptimeEnv *env) {
  struct Decl *decl = expr->ident.resolved;
  if (!decl)
    return sema_eval_normal(sema_const_invalid());

  struct ConstValue env_val;
  if (sema_comptime_env_lookup(env, decl, &env_val))
    return sema_eval_normal(env_val);

  if (decl->semantic_kind == SEM_TYPE) {
    struct Type *t = sema_type_of_decl(s, decl);
    if (!t || sema_type_is_errorish(t))
      return sema_eval_normal(sema_const_invalid());
    return sema_eval_normal(sema_const_type(t));
  }

  if (decl->kind == DECL_PRIMITIVE) {
    if (decl->name.string_id == s->name_true)
      return sema_eval_normal(sema_const_bool(true));
    if (decl->name.string_id == s->name_false)
      return sema_eval_normal(sema_const_bool(false));
  }

  if (decl->semantic_kind == SEM_VALUE && decl->node &&
      decl->node->kind == expr_Bind) {
    struct ConstValue cached = sema_decl_value(s, decl);
    if (cached.kind != CONST_INVALID) {
      return sema_eval_normal(cached);
    }

    // NEW: cycle guard. If this decl is currently being folded, bail.
    struct SemaDeclInfo *info = sema_decl_info(s, decl);
    if (info && info->fold_in_progress) {
      return sema_eval_normal(sema_const_invalid());
    }

    struct Expr *bind_value = decl->node->bind.value;
    if (bind_value) {
      // First-class function references are always comptime-usable,
      // regardless of how the binding itself was declared.
      if (bind_value->kind == expr_Lambda) {
        return sema_eval_normal(sema_const_function(decl));
      }
      // Only fold non-function bindings when the decl is comptime
      // (i.e. `::` form, or anything resolver/parser flagged). A `:=`
      // (runtime) binding has no compile-time value — its initializer
      // shouldn't be folded as if it were the binding's identity, since
      // later mutations would invalidate that.
      if (decl->is_comptime) {
        return sema_const_eval_expr(s, bind_value, env);
      }
    }
  }

  return sema_eval_normal(sema_const_invalid());
}

static struct EvalResult eval_bin(struct Sema *s, struct Expr *expr,
                                  struct ComptimeEnv *env) {
  struct EvalResult lr = sema_const_eval_expr(s, expr->bin.Left, env);
  if (lr.control != EVAL_NORMAL)
    return lr;

  struct EvalResult rr = sema_const_eval_expr(s, expr->bin.Right, env);
  if (rr.control != EVAL_NORMAL)
    return rr;

  struct ConstValue l = lr.value;
  struct ConstValue r = rr.value;
  if (l.kind == CONST_INVALID || r.kind == CONST_INVALID)
    return sema_eval_normal(sema_const_invalid());

  if (l.kind == CONST_FLOAT && r.kind == CONST_FLOAT) {
    switch (expr->bin.op) {
    case Plus:
      return sema_eval_normal(sema_const_float(l.float_val + r.float_val));
    case Minus:
      return sema_eval_normal(sema_const_float(l.float_val - r.float_val));
    case Star:
      return sema_eval_normal(sema_const_float(l.float_val * r.float_val));
    case ForwardSlash:
      if (r.float_val == 0.0)
        return sema_eval_normal(sema_const_invalid());
      return sema_eval_normal(sema_const_float(l.float_val / r.float_val));
    case EqualEqual:
      return sema_eval_normal(sema_const_bool(l.float_val == r.float_val));
    case BangEqual:
      return sema_eval_normal(sema_const_bool(l.float_val != r.float_val));
    case Less:
      return sema_eval_normal(sema_const_bool(l.float_val < r.float_val));
    case LessEqual:
      return sema_eval_normal(sema_const_bool(l.float_val <= r.float_val));
    case Greater:
      return sema_eval_normal(sema_const_bool(l.float_val > r.float_val));
    case GreaterEqual:
      return sema_eval_normal(sema_const_bool(l.float_val >= r.float_val));
    default:
      break;
    }
    return sema_eval_normal(sema_const_invalid());
  }

  if (l.kind == CONST_INT && r.kind == CONST_INT) {
    switch (expr->bin.op) {
    case Plus:
      return sema_eval_normal(sema_const_int(l.int_val + r.int_val));
    case Minus:
      return sema_eval_normal(sema_const_int(l.int_val - r.int_val));
    case Star:
      return sema_eval_normal(sema_const_int(l.int_val * r.int_val));
    case ForwardSlash:
      if (r.int_val == 0)
        return sema_eval_normal(sema_const_invalid());
      return sema_eval_normal(sema_const_int(l.int_val / r.int_val));
    case Percent:
      if (r.int_val == 0)
        return sema_eval_normal(sema_const_invalid());
      return sema_eval_normal(sema_const_int(l.int_val % r.int_val));
    case Ampersand:
      return sema_eval_normal(sema_const_int(l.int_val & r.int_val));
    case Pipe:
      return sema_eval_normal(sema_const_int(l.int_val | r.int_val));
    case Caret:
      return sema_eval_normal(sema_const_int(l.int_val ^ r.int_val));
    case ShiftLeft:
      return sema_eval_normal(sema_const_int(l.int_val << r.int_val));
    case ShiftRight:
      return sema_eval_normal(sema_const_int(l.int_val >> r.int_val));
    case EqualEqual:
      return sema_eval_normal(sema_const_bool(l.int_val == r.int_val));
    case BangEqual:
      return sema_eval_normal(sema_const_bool(l.int_val != r.int_val));
    case Less:
      return sema_eval_normal(sema_const_bool(l.int_val < r.int_val));
    case LessEqual:
      return sema_eval_normal(sema_const_bool(l.int_val <= r.int_val));
    case Greater:
      return sema_eval_normal(sema_const_bool(l.int_val > r.int_val));
    case GreaterEqual:
      return sema_eval_normal(sema_const_bool(l.int_val >= r.int_val));
    default:
      break;
    }
  }

  if (l.kind == CONST_BOOL && r.kind == CONST_BOOL) {
    switch (expr->bin.op) {
    case AmpersandAmpersand:
      return sema_eval_normal(sema_const_bool(l.bool_val && r.bool_val));
    case PipePipe:
      return sema_eval_normal(sema_const_bool(l.bool_val || r.bool_val));
    case EqualEqual:
      return sema_eval_normal(sema_const_bool(l.bool_val == r.bool_val));
    case BangEqual:
      return sema_eval_normal(sema_const_bool(l.bool_val != r.bool_val));
    default:
      break;
    }
  }

  if (l.kind == CONST_STRING && r.kind == CONST_STRING) {
    switch (expr->bin.op) {
    case EqualEqual:
      return sema_eval_normal(sema_const_bool(l.string_id == r.string_id));
    case BangEqual:
      return sema_eval_normal(sema_const_bool(l.string_id != r.string_id));
    default:
      break;
    }
  }

  return sema_eval_normal(sema_const_invalid());
}

static struct EvalResult eval_unary(struct Sema *s, struct Expr *expr,
                                    struct ComptimeEnv *env) {
  struct EvalResult v = sema_const_eval_expr(s, expr->unary.operand, env);
  if (v.control != EVAL_NORMAL)
    return v;
  if (v.value.kind == CONST_INVALID)
    return sema_eval_normal(sema_const_invalid());

  switch (expr->unary.op) {
  case unary_Neg:
    if (v.value.kind == CONST_INT)
      return sema_eval_normal(sema_const_int(-v.value.int_val));
    if (v.value.kind == CONST_FLOAT)
      return sema_eval_normal(sema_const_float(-v.value.float_val));
    return sema_eval_normal(sema_const_invalid());
  case unary_BitNot:
    if (v.value.kind == CONST_INT)
      return sema_eval_normal(sema_const_int(~v.value.int_val));
    return sema_eval_normal(sema_const_invalid());
  case unary_Not:
    if (v.value.kind == CONST_BOOL)
      return sema_eval_normal(sema_const_bool(!v.value.bool_val));
    return sema_eval_normal(sema_const_invalid());
  case unary_Inc: {
    struct Expr *op = expr->unary.operand;
    if (!op || op->kind != expr_Ident || !op->ident.resolved) {
      return sema_eval_err();
    }
    struct EvalResult vr = sema_const_eval_expr(s, op, env);
    if (vr.control != EVAL_NORMAL)
      return vr;
    if (vr.value.kind != CONST_INT) {
      return sema_eval_normal(sema_const_invalid());
    }

    struct ConstValue new_val = sema_const_int(vr.value.int_val + 1);

    sema_comptime_env_assign(s, env, op->ident.resolved, new_val);

    return sema_eval_normal(expr->unary.postfix ? vr.value : new_val);
  }
  default:
    return sema_eval_normal(sema_const_invalid());
  }
}

static struct EvalResult eval_block(struct Sema *s, struct Expr *expr,
                                    struct ComptimeEnv *env) {
  if (expr->block.stmts->count == 0) {
    return sema_eval_normal(sema_const_void());
  }

  struct ComptimeEnv *block_env = sema_comptime_env_new(s, env);

  struct EvalResult last = sema_eval_normal(sema_const_void());
  for (size_t i = 0; i < expr->block.stmts->count; i++) {
    struct Expr **stmt_p = (struct Expr **)vec_get(expr->block.stmts, i);
    struct Expr *stmt = stmt_p ? *stmt_p : NULL;
    if (!stmt)
      continue;

    last = sema_const_eval_expr(s, stmt, block_env);
    if (last.control != EVAL_NORMAL)
      return last;
  }
  return last;
}

static struct EvalResult eval_switch(struct Sema *s, struct Expr *expr,
                                     struct ComptimeEnv *env) {
  // Fold the scrutinee. If it doesn't fold, we can't pick an arm.
  struct EvalResult sr =
      sema_const_eval_expr(s, expr->switch_expr.scrutinee, env);
  if (sr.control != EVAL_NORMAL)
    return sr;
  if (sr.value.kind == CONST_INVALID) {
    return sema_eval_normal(sema_const_invalid());
  }

  // Walk arms; first matching arm wins. An arm matches if any of its patterns
  // folds to a value equal to the scrutinee's.
  if (expr->switch_expr.arms) {
    for (size_t i = 0; i < expr->switch_expr.arms->count; i++) {
      struct SwitchArm *arm =
          (struct SwitchArm *)vec_get(expr->switch_expr.arms, i);
      if (!arm || !arm->patterns)
        continue;

      for (size_t j = 0; j < arm->patterns->count; j++) {
        struct Expr **pat_p = (struct Expr **)vec_get(arm->patterns, j);
        struct Expr *pat = pat_p ? *pat_p : NULL;
        if (!pat)
          continue;

        struct EvalResult pr = sema_const_eval_expr(s, pat, env);
        if (pr.control != EVAL_NORMAL)
          continue;

        if (sema_const_value_equal(pr.value, sr.value)) {
          // Match — evaluate the arm body, return its value.
          return sema_const_eval_expr(s, arm->body, env);
        }
      }
    }
  }

  // No arm matched. Returning invalid is safer than erroring here — the
  // type-checker side already emitted "no matching arm" if applicable.
  return sema_eval_normal(sema_const_invalid());
}

static struct EvalResult eval_call(struct Sema *s, struct Expr *expr,
                                   struct ComptimeEnv *env) {
  // 1. Evaluate the callee - must yield CONST_FUNCTION
  struct EvalResult cr = sema_const_eval_expr(s, expr->call.callee, env);
  if (cr.control != EVAL_NORMAL)
    return cr;
  if (cr.value.kind != CONST_FUNCTION || !cr.value.fn_decl) {
    return sema_eval_normal(sema_const_invalid());
  }
  struct Decl *fn_decl = cr.value.fn_decl;

  // 2. Layer 1: the Decl's AST node should be a Bind (the `::` declaration).
  if (!fn_decl->node || fn_decl->node->kind != expr_Bind) {
    return sema_eval_normal(sema_const_invalid());
  }

  // 2. Layer 2: the Bind's value should be a Lambda.
  struct Expr *bind_value = fn_decl->node->bind.value;
  if (!bind_value || bind_value->kind != expr_Lambda) {
    return sema_eval_normal(sema_const_invalid());
  }
  Vec *params = bind_value->lambda.params;
  struct Expr *body = bind_value->lambda.body;
  if (!body) {
    return sema_eval_normal(sema_const_invalid());
  }

  // 3. Arity check.
  size_t param_count = params ? params->count : 0;
  size_t arg_count = expr->call.args ? expr->call.args->count : 0;
  if (param_count != arg_count) {
    return sema_eval_err();
  }

  struct ComptimeArgTuple *arg_values =
      arena_alloc(s->arena, sizeof(struct ComptimeArgTuple));
  arg_values->values = vec_new_in(s->arena, sizeof(struct ConstValue));

  for (size_t i = 0; i < arg_count; i++) {
    struct Expr **arg_p = (struct Expr **)vec_get(expr->call.args, i);
    struct EvalResult ar = sema_const_eval_expr(s, *arg_p, env);
    if (ar.control != EVAL_NORMAL)
      return ar; // bubble through
    vec_push(arg_values->values, &ar.value);
  }

  // 4.5: Cache lookup. If we've evaluated this exact (fn, args) before,
  // short-circuit and return the cached value.
  struct ConstValue cached;
  if (call_cache_lookup(s, fn_decl, arg_values, &cached)) {
    return sema_eval_normal(cached);
  }

  // 5. Recursion guard.
  if (s->comptime_call_depth >= ORE_COMPTIME_CALL_DEPTH_MAX) {
    return sema_eval_err();
  }

  s->comptime_call_depth++;
  s->comptime_body_evals++;

  // 6. Build a fresh env. parent=NULL - callees only see their own params and
  // module-level decls
  struct ComptimeEnv *call_env = sema_comptime_env_new(s, NULL);

  // 7. Bind each param's Decl to it's evaluated arg.
  for (size_t i = 0; i < param_count; i++) {
    struct Param *p = (struct Param *)vec_get(params, i);
    struct ConstValue *v = (struct ConstValue *)vec_get(arg_values->values, i);
    if (p && p->name.resolved && v) {
      sema_comptime_env_bind(s, call_env, p->name.resolved, *v);
    }
  }

  // 8. Evaluate the body in the new frame.
  struct EvalResult result = sema_const_eval_expr(s, body, call_env);

  // 9-11. Centralized cleanup: every exit path from here decrements the
  // call-depth counter and (on success) populates the cache. This is what
  // keeps `comptime_call_depth` from drifting on break/continue exits and
  // ensures cache stores aren't accidentally skipped by future control-flow
  // additions. Don't add early returns past this point — fall through to
  // `done`.
  switch (result.control) {
  case EVAL_RETURN:
    // RETURN at the function boundary becomes a NORMAL value.
    result.control = EVAL_NORMAL;
    break;
  case EVAL_BREAK:
  case EVAL_CONTINUE:
    // break/continue can't escape a function — convert to error.
    result = sema_eval_err();
    break;
  case EVAL_NORMAL:
  case EVAL_ERROR:
    break;
  }

  if (result.control == EVAL_NORMAL) {
    call_cache_store(s, fn_decl, arg_values, result.value);
  }

  s->comptime_call_depth--;
  return result;
}

static struct EvalResult eval_loop(struct Sema *s, struct Expr *expr,
                                   struct ComptimeEnv *env) {
  // Loops introduce a fresh scope so `i := 0` doesn't leak to the outer
  // env. Init / condition / step / body all share this scope.
  struct ComptimeEnv *loop_env = sema_comptime_env_new(s, env);

  // Init clause runs once before the first condition check.
  if (expr->loop_expr.init) {
    struct EvalResult ir =
        sema_const_eval_expr(s, expr->loop_expr.init, loop_env);
    if (ir.control != EVAL_NORMAL)
      return ir; // bubble RETURN/ERROR
  }

  // Fuel: every loop body counts as one tick. Without this, an infinite
  // loop like `loop` with no break would spin forever.
  int64_t fuel = ORE_COMPTIME_LOOP_FUEL;

  for (;;) {
    if (fuel-- <= 0) {
      sema_error(s, expr->span, "comptime loop exceeded fuel (%lld iterations)",
                 (long long)ORE_COMPTIME_LOOP_FUEL);
      return sema_eval_err();
    }

    // Condition check (NULL means always-true / infinite loop).
    if (expr->loop_expr.condition) {
      struct EvalResult cr =
          sema_const_eval_expr(s, expr->loop_expr.condition, loop_env);
      if (cr.control != EVAL_NORMAL)
        return cr;
      if (cr.value.kind != CONST_BOOL) {
        return sema_eval_normal(sema_const_invalid());
      }
      if (!cr.value.bool_val)
        break; // condition false → exit
    }

    // Body.
    struct EvalResult br =
        sema_const_eval_expr(s, expr->loop_expr.body, loop_env);
    switch (br.control) {
    case EVAL_NORMAL:
      break; // continue to step clause
    case EVAL_CONTINUE:
      break; // same: fall through to step
    case EVAL_BREAK:
      goto loop_end; // exit loop, no value bubbles
    case EVAL_RETURN:
      return br; // bubble up
    case EVAL_ERROR:
      return br; // bubble up
    }

    // Step clause (e.g., `i++`).
    if (expr->loop_expr.step) {
      struct EvalResult sr =
          sema_const_eval_expr(s, expr->loop_expr.step, loop_env);
      if (sr.control != EVAL_NORMAL)
        return sr;
    }
  }

loop_end:
  // Loops yield void — they're statements, not value-producing expressions.
  return sema_eval_normal(sema_const_void());
}

static struct EvalResult eval_if(struct Sema *s, struct Expr *expr,
                                 struct ComptimeEnv *env) {
  // Evaluate the condition. If it's not foldable to a bool, this branch can't
  // be picked at comptime -- CONST_INVALD
  struct EvalResult cr = sema_const_eval_expr(s, expr->if_expr.condition, env);
  if (cr.control != EVAL_NORMAL)
    return cr;
  if (cr.value.kind != CONST_BOOL)
    return sema_eval_normal(sema_const_invalid());

  // Pick the branch. The unselected branch is NEVER evaluated.
  if (cr.value.bool_val) {
    return sema_const_eval_expr(s, expr->if_expr.then_branch, env);
  }
  if (expr->if_expr.else_branch) {
    return sema_const_eval_expr(s, expr->if_expr.else_branch, env);
  }

  // no else branch and condition was false → result is void
  return sema_eval_normal(sema_const_void());
}

static struct EvalResult eval_return(struct Sema *s, struct Expr *expr,
                                     struct ComptimeEnv *env) {
  if (!expr->return_expr.value) {
    return (struct EvalResult){
        .control = EVAL_RETURN,
        .value = sema_const_void(),
    };
  }

  // return X -> eval X, propagate any non-normal control, otherwise tag value
  // with RETURN
  struct EvalResult v = sema_const_eval_expr(s, expr->return_expr.value, env);
  if (v.control != EVAL_NORMAL)
    return v;
  return (struct EvalResult){
      .control = EVAL_RETURN,
      .value = v.value,
  };
}

static struct EvalResult eval_break(struct Sema *s, struct Expr *expr,
                                    struct ComptimeEnv *env) {
  (void)s;
  (void)expr;
  (void)env;
  return (struct EvalResult){.control = EVAL_BREAK, .value = sema_const_void()};
}

static struct EvalResult eval_continue(struct Sema *s, struct Expr *expr,
                                       struct ComptimeEnv *env) {
  (void)s;
  (void)expr;
  (void)env;
  return (struct EvalResult){
      .control = EVAL_CONTINUE,
      .value = sema_const_void(),
  };
}

static struct EvalResult eval_product(struct Sema *s, struct Expr *expr,
                                      struct ComptimeEnv *env) {
  // Untyped product literals (`.{ ... }`) need bidirectional context to
  // know which struct they're inhabiting. Without that context, we can't
  // produce a CONST_STRUCT.
  if (!expr->product.type_expr) {
    return sema_eval_normal(sema_const_invalid());
  }

  // Evaluate the type expression — must yield CONST_TYPE.
  struct EvalResult tr = sema_const_eval_expr(s, expr->product.type_expr, env);
  if (tr.control != EVAL_NORMAL)
    return tr;
  if (tr.value.kind != CONST_TYPE || !tr.value.type_val) {
    return sema_eval_normal(sema_const_invalid());
  }
  struct Type *struct_type = tr.value.type_val;

  // Build the ConstStruct in the arena.
  struct ConstStruct *sv = arena_alloc(s->arena, sizeof(struct ConstStruct));
  sv->type = struct_type;
  sv->fields = vec_new_in(s->arena, sizeof(struct ConstStructField));

  // Walk the literal's fields, evaluate each value.
  if (expr->product.Fields) {
    for (size_t i = 0; i < expr->product.Fields->count; i++) {
      struct ProductField *f =
          (struct ProductField *)vec_get(expr->product.Fields, i);
      if (!f)
        continue;
      if (f->is_spread) {
        // Spread (`..other`) is out of scope for this step.
        return sema_eval_normal(sema_const_invalid());
      }

      struct EvalResult vr = sema_const_eval_expr(s, f->value, env);
      if (vr.control != EVAL_NORMAL)
        return vr; // bubble up

      struct ConstStructField cf = {
          .name_id = f->name.string_id,
          .value = vr.value,
      };
      vec_push(sv->fields, &cf);
    }
  }

  return sema_eval_normal(sema_const_struct(sv));
}

static struct EvalResult eval_field(struct Sema *s, struct Expr *expr,
                                    struct ComptimeEnv *env) {
  // Existing: @target.os/.arch/.pointer_size special-case.
  const char *tfield = NULL;
  if (is_target_field_chain(s, expr, &tfield)) {
    return eval_target_field(s, tfield);
  }

  // Evaluate the object — must yield a CONST_STRUCT to read fields.
  struct EvalResult obj = sema_const_eval_expr(s, expr->field.object, env);
  if (obj.control != EVAL_NORMAL)
    return obj;

  if (obj.value.kind != CONST_STRUCT || !obj.value.struct_val) {
    return sema_eval_normal(sema_const_invalid());
  }

  // Find the field by name_id.
  uint32_t target_name = expr->field.field.string_id;
  Vec *fields = obj.value.struct_val->fields;
  for (size_t i = 0; i < fields->count; i++) {
    struct ConstStructField *f = (struct ConstStructField *)vec_get(fields, i);
    if (f && f->name_id == target_name) {
      return sema_eval_normal(f->value);
    }
  }

  // Field not found — return invalid (the type checker should have rejected
  // upstream, but we don't crash).
  return sema_eval_normal(sema_const_invalid());
}

static struct EvalResult eval_array_lit(struct Sema *s, struct Expr *expr,
                                        struct ComptimeEnv *env) {
  // Resolve element type if present.
  struct Type *elem_type = NULL;
  if (expr->array_lit.elem_type) {
    struct EvalResult tr =
        sema_const_eval_expr(s, expr->array_lit.elem_type, env);
    if (tr.control == EVAL_NORMAL && tr.value.kind == CONST_TYPE) {
      elem_type = tr.value.type_val;
    }
  }

  struct ConstArray *av = arena_alloc(s->arena, sizeof(struct ConstArray));
  av->elem_type = elem_type;
  av->elements = vec_new_in(s->arena, sizeof(struct ConstValue));

  // Initializer is the `{...}` part. If it's a positional product literal,
  // pull each field's value out and append.
  struct Expr *init = expr->array_lit.initializer;
  if (init && init->kind == expr_Product && init->product.Fields) {
    for (size_t i = 0; i < init->product.Fields->count; i++) {
      struct ProductField *f =
          (struct ProductField *)vec_get(init->product.Fields, i);
      if (!f)
        continue;
      if (f->is_spread) {
        // Skip spreads for now — same scope as step 10.
        return sema_eval_normal(sema_const_invalid());
      }

      struct EvalResult vr = sema_const_eval_expr(s, f->value, env);
      if (vr.control != EVAL_NORMAL)
        return vr;
      vec_push(av->elements, &vr.value);
    }
  } else {
    // Non-product initializer (e.g. a single value or a spread): not
    // foldable in the current shape. Type checking handles size /
    // element-type mismatches separately.
    return sema_eval_normal(sema_const_invalid());
  }

  return sema_eval_normal(sema_const_array(av));
}

static struct EvalResult eval_index(struct Sema *s, struct Expr *expr,
                                    struct ComptimeEnv *env) {
  // Evaluate the object — must yield CONST_ARRAY.
  struct EvalResult obj = sema_const_eval_expr(s, expr->index.object, env);
  if (obj.control != EVAL_NORMAL)
    return obj;
  if (obj.value.kind != CONST_ARRAY || !obj.value.array_val) {
    return sema_eval_normal(sema_const_invalid());
  }

  // Evaluate the index — must yield CONST_INT.
  struct EvalResult idx = sema_const_eval_expr(s, expr->index.index, env);
  if (idx.control != EVAL_NORMAL)
    return idx;
  if (idx.value.kind != CONST_INT) {
    return sema_eval_normal(sema_const_invalid());
  }

  Vec *elements = obj.value.array_val->elements;
  if (!elements)
    return sema_eval_normal(sema_const_invalid());

  // Bounds check.
  int64_t i = idx.value.int_val;
  if (i < 0 || (uint64_t)i >= elements->count) {
    // Out-of-bounds — emit an error since the source is broken.
    sema_error(s, expr->span, "comptime array index out of bounds");
    return sema_eval_err();
  }

  struct ConstValue *v = (struct ConstValue *)vec_get(elements, (size_t)i);
  if (!v)
    return sema_eval_normal(sema_const_invalid());
  return sema_eval_normal(*v);
}

struct EvalResult sema_const_eval_expr(struct Sema *s, struct Expr *expr,
                                       struct ComptimeEnv *env) {
  if (!s || !expr)
    return sema_eval_normal(sema_const_invalid());

  switch (expr->kind) {
  case expr_Lit:
    return eval_lit(s, expr);
  case expr_Ident:
    return eval_ident(s, expr, env);
  case expr_Bin:
    return eval_bin(s, expr, env);
  case expr_Unary:
    return eval_unary(s, expr, env);
  case expr_Builtin:
    return eval_builtin(s, expr, env);
  case expr_Field:
    return eval_field(s, expr, env);
  case expr_ArrayLit:
    return eval_array_lit(s, expr, env);
  case expr_Index:
    return eval_index(s, expr, env);
  case expr_Switch:
    return eval_switch(s, expr, env);
  case expr_Bind: {
    if (!expr->bind.value) {
      return sema_eval_normal(sema_const_void());
    }

    struct EvalResult vr = sema_const_eval_expr(s, expr->bind.value, env);
    if (vr.control != EVAL_NORMAL)
      return vr;

    if (env && expr->bind.name.resolved) {
      sema_comptime_env_bind(s, env, expr->bind.name.resolved, vr.value);
    }

    return vr;
  }
  case expr_Assign: {
    // Evaluate the RHS first.
    struct EvalResult rhs = sema_const_eval_expr(s, expr->assign.value, env);
    if (rhs.control != EVAL_NORMAL)
      return rhs;

    struct Expr *tgt = expr->assign.target;
    if (!tgt || tgt->kind != expr_Ident || !tgt->ident.resolved) {
      return sema_eval_err();
    }

    sema_comptime_env_assign(s, env, tgt->ident.resolved, rhs.value);

    return sema_eval_normal(sema_const_void());
  }
  case expr_Block:
    return eval_block(s, expr, env);
  case expr_Return:
    return eval_return(s, expr, env);
  case expr_Continue:
    return eval_continue(s, expr, env);
  case expr_Break:
    return eval_break(s, expr, env);
  case expr_If:
    return eval_if(s, expr, env);
  case expr_Loop:
    return eval_loop(s, expr, env);
  case expr_Call:
    return eval_call(s, expr, env);
  case expr_Product:
    return eval_product(s, expr, env);

  // ---- Unfoldable kinds (intentionally enumerated) ----
  //
  // These don't have a meaningful comptime *value* in our model:
  // - Type-shape nodes (Lambda, Ctl, Struct, Enum, Effect, EffectRow,
  //   ArrayType, SliceType, ManyPtrType) describe types, not values.
  //   Type-position evaluation goes through `sema_infer_type_expr`
  //   instead.
  // - Effect-flow nodes (With, Defer) are statement-shaped with
  //   side effects we can't model at comptime.
  // - Pattern-only nodes (EnumRef, Wildcard, DestructureBind)
  //   appear in match arms / destructure targets, not evaluatable
  //   positions.
  // - Asm is opaque to the interpreter by design.
  //
  // Enumerating them explicitly (rather than falling through to a
  // catch-all default) lets `-Wswitch` flag any *new* kind added to
  // `enum ExprKind` so we consider whether it's foldable.
  case expr_Lambda:
  case expr_Ctl:
  case expr_Handler:
  case expr_With:
  case expr_Struct:
  case expr_Enum:
  case expr_Effect:
  case expr_EffectRow:
  case expr_ArrayType:
  case expr_SliceType:
  case expr_ManyPtrType:
  case expr_Defer:
  case expr_EnumRef:
  case expr_Wildcard:
  case expr_DestructureBind:
  case expr_Asm:
    return sema_eval_normal(sema_const_invalid());
  }
  return sema_eval_normal(sema_const_invalid());
}
