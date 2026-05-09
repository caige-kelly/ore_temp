#include "decl_data.h"

#include <string.h>

#include "../../common/arena.h"
#include "../../common/hashmap.h"
#include "../../common/vec.h"
#include "../../diag/diag.h"
#include "../../parser/ast.h"
#include "../query/query_engine.h"
#include "../sema.h"
#include "checker.h"     // resolve_type_expr
#include "type.h"

// =====================================================================
// FnSignature
// =====================================================================

static struct FnSignature *
fn_signature_entry_for(struct Sema *s, DefId fn_def) {
  if (s->fn_signatures.entries == NULL)
    hashmap_init_in(&s->fn_signatures, &s->arena);

  uint64_t key = (uint64_t)fn_def.idx;
  if (hashmap_contains(&s->fn_signatures, key))
    return (struct FnSignature *)hashmap_get(&s->fn_signatures, key);

  struct FnSignature *sig = arena_alloc(&s->arena, sizeof(*sig));
  *sig = (struct FnSignature){0};
  sema_query_slot_init(&sig->query, QUERY_FN_SIGNATURE);
  hashmap_put_or_die(&s->fn_signatures, key, sig, "fn_signatures");
  return sig;
}

// Pull the Lambda Expr out of a fn's owning Bind, or NULL if the
// def isn't fn-shaped. Diagnostics for the wrong-shape case are the
// caller's concern.
static struct Expr *fn_lambda_for_def(struct Sema *s, DefId fn_def) {
  struct DefInfo *di = def_info(s, fn_def);
  if (!di || di->kind != DECL_USER || !di->origin) return NULL;
  if (di->origin->kind != expr_Bind) return NULL;
  struct Expr *value = di->origin->bind.value;
  if (!value || value->kind != expr_Lambda) return NULL;
  return value;
}

struct FnSignature *query_fn_signature(struct Sema *s, DefId fn_def) {
  if (!s || !def_id_is_valid(fn_def)) return NULL;
  struct Expr *lambda = fn_lambda_for_def(s, fn_def);
  if (!lambda) return NULL;

  struct FnSignature *sig = fn_signature_entry_for(s, fn_def);

  struct Span frame_span = lambda->span;
  SEMA_QUERY_GUARD(s, &sig->query, QUERY_FN_SIGNATURE, sig, frame_span,
                   /*on_cached=*/sig,
                   /*on_cycle=*/NULL,
                   /*on_error=*/NULL);

  // Resolve param types. Allocate parallel arrays in the arena.
  size_t n = lambda->lambda.params ? lambda->lambda.params->count : 0;
  struct Type **param_types = NULL;
  ParamKind *param_kinds = NULL;
  if (n > 0) {
    param_types = arena_alloc(&s->arena, sizeof(struct Type *) * n);
    param_kinds = arena_alloc(&s->arena, sizeof(ParamKind) * n);
    for (size_t i = 0; i < n; i++) {
      struct Param *p = (struct Param *)vec_get(lambda->lambda.params, i);
      if (!p) {
        param_types[i] = s->error_type;
        param_kinds[i] = PARAM_RUNTIME;
        continue;
      }
      if (!p->type_ann) {
        diag_error(&s->diags, lambda->span,
                   "function parameter #%zu requires a type annotation", i);
        param_types[i] = s->error_type;
      } else {
        param_types[i] = resolve_type_expr(s, p->type_ann);
      }
      param_kinds[i] = p->kind;
    }
  }

  struct Type *ret_type = lambda->lambda.ret_type
                              ? resolve_type_expr(s, lambda->lambda.ret_type)
                              : s->void_type;

  sig->param_types = param_types;
  sig->param_kinds = param_kinds;
  sig->param_count = n;
  sig->ret_type = ret_type;
  sig->is_comptime = lambda->is_comptime;
  sig->has_effects = lambda->lambda.effect != NULL;

  // Fingerprint over the structurally meaningful contents. Since
  // Type*s are interned, hashing pointer addresses gives type-shape
  // equality. param_kinds and modifier bits get folded in too.
  Fingerprint fp = query_fingerprint_from_u64(n);
  for (size_t i = 0; i < n; i++) {
    fp = query_fingerprint_combine(
        fp, query_fingerprint_from_pointer(param_types[i]));
    fp = query_fingerprint_combine(fp,
                                   query_fingerprint_from_u64(param_kinds[i]));
  }
  fp = query_fingerprint_combine(fp, query_fingerprint_from_pointer(ret_type));
  uint64_t modifiers = ((uint64_t)sig->is_comptime << 1) |
                       (uint64_t)sig->has_effects;
  fp = query_fingerprint_combine(fp, query_fingerprint_from_u64(modifiers));
  query_slot_set_fingerprint(&sig->query, fp);

  sema_query_succeed(s, &sig->query);
  return sig;
}

// =====================================================================
// ParamLocator
// =====================================================================

void param_locator_set(struct Sema *s, DefId param_def, DefId parent_fn,
                       uint32_t index) {
  if (!s || !def_id_is_valid(param_def)) return;
  if (s->param_locators.entries == NULL)
    hashmap_init_in(&s->param_locators, &s->arena);

  uint64_t key = (uint64_t)param_def.idx;
  struct ParamLocator *loc;
  if (hashmap_contains(&s->param_locators, key)) {
    loc = (struct ParamLocator *)hashmap_get(&s->param_locators, key);
  } else {
    loc = arena_alloc(&s->arena, sizeof(struct ParamLocator));
    hashmap_put_or_die(&s->param_locators, key, loc, "param_locators");
  }
  loc->parent_fn = parent_fn;
  loc->index = index;
}

struct ParamLocator *param_locator_get(struct Sema *s, DefId param_def) {
  if (!s || !def_id_is_valid(param_def)) return NULL;
  if (s->param_locators.entries == NULL) return NULL;
  uint64_t key = (uint64_t)param_def.idx;
  if (!hashmap_contains(&s->param_locators, key)) return NULL;
  return (struct ParamLocator *)hashmap_get(&s->param_locators, key);
}
