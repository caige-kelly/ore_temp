#include "effect_ops.h"

#include <stddef.h>

#include "../../common/hashmap.h"
#include "../../common/vec.h"
#include "../../parser/ast.h"
#include "../scope/scope.h"
#include "../sema.h"
#include "resolve.h"

// Walk an effect-annotation AST subtree and append every op
// DefId reachable through it to `out`.
//
// The annotation grammar is roughly:
//   eff_expr   ::= Ident                  // a single named effect
//                | EffectRow              // <head | row>
//                | union of any of these
//
// For the simple Ident case: resolve to the effect DefId,
// then iterate its child_scope's defs and append each op.
// For EffectRow: recurse into the head (the row variable
// doesn't expose ops since it's open).
//
// Other forms (parenthesized, comma-separated multi-effect
// annotations) are deferred — current parser produces them as
// expr_Ident or expr_EffectRow at the leaf level. If a future
// parser change introduces a new shape, extend the switch.
static void collect_from_annotation(struct Sema *s, struct Expr *ann,
                                    Vec *out) {
  if (!ann)
    return;

  switch (ann->kind) {
  case expr_Ident: {
    DefId eff_def = query_resolve_ref(s, ann, NS_EFFECT);
    if (!def_id_is_valid(eff_def))
      return;
    struct DefInfo *di = def_info(s, eff_def);
    if (!di || di->semantic_kind != SEM_EFFECT)
      return;
    struct ScopeInfo *eff_scope = scope_info(s, di->child_scope);
    if (!eff_scope || !eff_scope->defs)
      return;
    for (size_t i = 0; i < eff_scope->defs->count; i++) {
      DefId *op = (DefId *)vec_get(eff_scope->defs, i);
      if (!op)
        continue;
      vec_push(out, op);
    }
    return;
  }

  case expr_EffectRow:
    // Closed head exposes its ops; the open row variable does
    // not (its ops are determined dynamically at the call
    // site).
    collect_from_annotation(s, ann->effect_row.head, out);
    return;

  default:
    // Unknown annotation shape; ignore rather than failing the
    // query. Bug if this fires often — extend the switch.
    return;
  }
}

Vec *query_effect_ops_visible(struct Sema *s, DefId fn_def) {
  if (!def_id_is_valid(fn_def))
    return NULL;

  // Cache hit?
  if (hashmap_contains(&s->effect_ops_cache, (uint64_t)fn_def.idx))
    return (Vec *)hashmap_get(&s->effect_ops_cache, (uint64_t)fn_def.idx);

  struct DefInfo *di = def_info(s, fn_def);
  if (!di || !di->origin)
    return NULL;

  // Find the effect annotation. We expect `fn_def->origin` to
  // be a Bind whose value is a Lambda with `effect`.
  struct Expr *ann = NULL;
  struct Expr *e = di->origin;
  if (e->kind == expr_Bind && e->bind.value &&
      e->bind.value->kind == expr_Lambda) {
    ann = e->bind.value->lambda.effect;
  } else if (e->kind == expr_Lambda) {
    ann = e->lambda.effect;
  }

  if (!ann) {
    hashmap_put(&s->effect_ops_cache, (uint64_t)fn_def.idx, NULL);
    return NULL;
  }

  Vec *ops = vec_new_in(s->arena, sizeof(DefId));
  collect_from_annotation(s, ann, ops);
  hashmap_put(&s->effect_ops_cache, (uint64_t)fn_def.idx, ops);
  return ops;
}
