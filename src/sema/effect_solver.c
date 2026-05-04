#include "effect_solver.h"

#include "../compiler/compiler.h"
#include "../diag/diag.h"
#include "../hir/hir.h"
#include "../parser/ast.h"
#include "effects.h"
#include "evidence.h"
#include "instantiate.h"
#include "sema.h"
#include "sema_internal.h"
#include "type.h"

// ----- EffectSet helpers -----

static struct EffectSet *effect_set_new(struct Sema *s) {
  if (!s || !s->arena)
    return NULL;
  struct EffectSet *set = arena_alloc(s->arena, sizeof(struct EffectSet));
  if (!set)
    return NULL;
  set->terms = vec_new_in(s->arena, sizeof(struct EffectTerm));
  set->open = false;
  set->open_row_name_id = 0;
  return set;
}

static bool effect_term_matches(const struct EffectTerm *a,
                                const struct EffectTerm *b) {
  if (!a || !b)
    return false;
  if (a->decl && b->decl)
    return a->decl == b->decl;
  if (a->name_id && b->name_id)
    return a->name_id == b->name_id;
  return false;
}

// TODO(perf): linear scan, called per add. Fine while typical sets are < 30
// terms; revisit with a per-set hashset keyed by (decl, scope_token_id) once
// real programs push us past that. Do not bother before then.
static bool effect_set_contains(struct EffectSet *set,
                                const struct EffectTerm *term) {
  if (!set || !set->terms || !term)
    return false;
  for (size_t i = 0; i < set->terms->count; i++) {
    struct EffectTerm *t = (struct EffectTerm *)vec_get(set->terms, i);
    if (effect_term_matches(t, term))
      return true;
  }
  return false;
}

static void effect_set_add(struct EffectSet *set, struct EffectTerm term) {
  if (!set || !set->terms)
    return;
  if (term.kind == EFFECT_TERM_UNKNOWN)
    return;
  if (effect_set_contains(set, &term))
    return;
  vec_push(set->terms, &term);
}

// Copy every term from `src` into `dst` *except* those that match
// `discharged`. Open-row state copies through unchanged. The standard
// dedup in `effect_set_add` handles overlap with terms already in `dst`.
//
// This is the discharge primitive for handlers: a handler that
// discharges effect E walks its body's effects, drops the E term, and
// contributes the rest to the surrounding scope's set.
static void effect_set_copy_minus(struct EffectSet *dst, struct EffectSet *src,
                                  struct Decl *discharged) {
  if (!dst || !src)
    return;
  if (src->terms) {
    for (size_t i = 0; i < src->terms->count; i++) {
      struct EffectTerm *t = (struct EffectTerm *)vec_get(src->terms, i);
      if (!t)
        continue;
      if (discharged && t->decl == discharged)
        continue; // dropped
      effect_set_add(dst, *t);
    }
  }
  if (src->open) {
    dst->open = true;
    if (dst->open_row_name_id == 0)
      dst->open_row_name_id = src->open_row_name_id;
  }
}

// True if `set` contains a term whose effect is `eff`. Used by handler
// discharge to detect dead handlers (the handler discharges an effect
// the body never performs).
static bool effect_set_contains_decl(struct EffectSet *set, struct Decl *eff) {
  if (!set || !set->terms || !eff)
    return false;
  for (size_t i = 0; i < set->terms->count; i++) {
    struct EffectTerm *t = (struct EffectTerm *)vec_get(set->terms, i);
    if (t && t->decl == eff)
      return true;
  }
  return false;
}

// Map an op decl back to the Decl of the effect E it belongs to.
// Phase F.2 deleted the DECL_EFFECT_OP synthetic shortcut; bare op
// references now resolve straight to the source DECL_FIELD whose
// owner is E's SCOPE_EFFECT. Climb owner → parent and find the decl
// whose child_scope is the op's owning SCOPE_EFFECT.
static struct Decl *effect_decl_for_op(struct Decl *op) {
  if (!op)
    return NULL;
  if (!op->owner || op->owner->kind != SCOPE_EFFECT)
    return NULL;
  struct Scope *eff_scope = op->owner;
  struct Scope *parent = eff_scope->parent;
  if (!parent || !parent->decls)
    return NULL;
  for (size_t i = 0; i < parent->decls->count; i++) {
    struct Decl **d_p = (struct Decl **)vec_get(parent->decls, i);
    struct Decl *d = d_p ? *d_p : NULL;
    if (d && d->child_scope == eff_scope && d->semantic_kind == SEM_EFFECT)
      return d;
  }
  return NULL;
}

// ----- Body walk helpers (shared with the HIR walker) -----

static void union_callee_effects(struct EffectSet *set, struct EffectSig *sig) {
  if (!set || !sig)
    return;
  if (sig->is_open) {
    set->open = true;
    if (set->open_row_name_id == 0)
      set->open_row_name_id = sig->row_name_id;
  }
  if (!sig->terms)
    return;
  for (size_t i = 0; i < sig->terms->count; i++) {
    struct EffectTerm *t = (struct EffectTerm *)vec_get(sig->terms, i);
    if (t)
      effect_set_add(set, *t);
  }
}

// Subtract every term in `discharge` from `src`, adding the rest to
// `dst`. Open-row state copies through. Used by signature-driven
// discharge to drop the action's declared effects from a body lambda's
// inferred effects.
static void effect_set_copy_minus_set(struct EffectSet *dst,
                                      struct EffectSet *src,
                                      struct EffectSig *discharge) {
  if (!dst || !src)
    return;
  if (src->terms) {
    for (size_t i = 0; i < src->terms->count; i++) {
      struct EffectTerm *t = (struct EffectTerm *)vec_get(src->terms, i);
      if (!t)
        continue;
      bool drop = false;
      if (discharge && discharge->terms) {
        for (size_t j = 0; j < discharge->terms->count; j++) {
          struct EffectTerm *d =
              (struct EffectTerm *)vec_get(discharge->terms, j);
          if (effect_term_matches(d, t)) {
            drop = true;
            break;
          }
        }
      }
      if (!drop)
        effect_set_add(dst, *t);
    }
  }
  if (src->open) {
    dst->open = true;
    if (dst->open_row_name_id == 0)
      dst->open_row_name_id = src->open_row_name_id;
  }
}

// ----- Body walk over HIR -----
//
// Walks the per-decl / per-instantiation HIR built by
// sema_lower_modules. The HIR shape carries everything the effect
// solver needs directly: HIR_CALL.callee_decl is set during lowering;
// HIR_HANDLER_INSTALL.effect_decl is direct; HIR_OP_PERFORM carries
// effect_decl + op_decl explicitly (no effect_decl_for_op round-trip).
// HIR_LAMBDA's body effects belong to its own HirFn (verified when
// that fn's enclosing decl is checked) — don't recurse, would
// double-count.
//
// Effekt-style semantics: union callee effects, subtract handler
// effects, action-shape discharge per param effect_sig.

static void collect_from_hir(struct Sema *s, struct EffectSet *set,
                             struct HirInstr *h);

static void collect_from_hir_block(struct Sema *s, struct EffectSet *set,
                                   Vec *block) {
  if (!block)
    return;
  for (size_t i = 0; i < block->count; i++) {
    struct HirInstr **ip = (struct HirInstr **)vec_get(block, i);
    if (ip && *ip)
      collect_from_hir(s, set, *ip);
  }
}

static void collect_from_hir_call(struct Sema *s, struct EffectSet *set,
                                  struct HirInstr *call) {
  if (!call)
    return;
  // Walk the callee operand for any effects in subexpressions
  // (rare — usually just a Ref).
  if (call->call.callee)
    collect_from_hir(s, set, call->call.callee);

  struct Decl *callee_decl = call->call.callee_decl;
  struct Type *ctype = callee_decl ? sema_decl_type(s, callee_decl) : NULL;

  // Action-shape discharge: for each lambda arg whose corresponding
  // callee param is fn-typed with an effect_sig, walk the lambda's
  // body and subtract the action's declared effects from the residual
  // before adding it to the outer set.
  Vec *ast_params = callee_decl ? sema_decl_function_params(callee_decl) : NULL;
  Vec *type_params = ctype ? ctype->params : NULL;
  bool walked_args = false;
  if (ast_params && call->call.args) {
    size_t arg_i = 0;
    for (size_t pi = 0; pi < ast_params->count; pi++) {
      struct Param *p = (struct Param *)vec_get(ast_params, pi);
      if (!p)
        continue;
      if (p->kind == PARAM_INFERRED_COMPTIME)
        continue;
      if (arg_i >= call->call.args->count)
        break;
      struct HirInstr **ap =
          (struct HirInstr **)vec_get(call->call.args, arg_i);
      struct HirInstr *arg = ap ? *ap : NULL;
      struct Type *param_ty = NULL;
      if (type_params && pi < type_params->count) {
        struct Type **tp = (struct Type **)vec_get(type_params, pi);
        param_ty = tp ? *tp : NULL;
      }
      arg_i++;
      if (!arg)
        continue;
      bool action_shape = arg->kind == HIR_LAMBDA && arg->lambda.fn &&
                          param_ty && param_ty->kind == TYPE_FUNCTION &&
                          param_ty->effect_sig;
      if (action_shape) {
        struct EffectSet *body_set = effect_set_new(s);
        collect_from_hir_block(s, body_set, arg->lambda.fn->body_block);
        effect_set_copy_minus_set(set, body_set, param_ty->effect_sig);
      } else {
        collect_from_hir(s, set, arg);
      }
    }
    walked_args = true;
  }
  if (!walked_args && call->call.args) {
    for (size_t i = 0; i < call->call.args->count; i++) {
      struct HirInstr **ip = (struct HirInstr **)vec_get(call->call.args, i);
      if (ip && *ip)
        collect_from_hir(s, set, *ip);
    }
  }

  if (callee_decl) {
    struct EffectSig *esig = sema_decl_effect_sig(s, callee_decl);
    if (esig)
      union_callee_effects(set, esig);
    else if (ctype && ctype->effect_sig) {
      union_callee_effects(set, ctype->effect_sig);
    }
    // Op call → implicitly performs the parent effect. After Phase F.1
    // bare op calls lower as HIR_OP_PERFORM and bypass this branch;
    // it stays for the named-form `x.op()` callee shape (HIR_CALL with
    // a HIR_FIELD callee whose resolved decl is a DECL_FIELD owned by
    // a SCOPE_EFFECT scope).
    struct Decl *eff = effect_decl_for_op(callee_decl);
    if (eff) {
      struct EffectTerm term = {
          .kind = EFFECT_TERM_NAMED,
          .expr = NULL, // span info now on HirInstr; AST
                        // pointer is stale post-lowering
          .decl = eff,
          .name_id = eff->name.string_id,
      };
      effect_set_add(set, term);
    }
  }
}

static void collect_from_hir(struct Sema *s, struct EffectSet *set,
                             struct HirInstr *h) {
  if (!h)
    return;
  switch (h->kind) {
  // Pure leaves and type-only kinds — no transitive Call reach.
  case HIR_CONST:
  case HIR_REF:
  case HIR_BREAK:
  case HIR_CONTINUE:
  case HIR_TYPE_VALUE:
  case HIR_ASM:
  case HIR_ENUM_REF:
  case HIR_ERROR:
    return;

  case HIR_CALL:
    collect_from_hir_call(s, set, h);
    return;

  case HIR_OP_PERFORM:
    // Phase F creates these. The instr carries effect_decl
    // directly — no effect_decl_for_op lookup needed.
    if (h->op_perform.effect_decl) {
      struct EffectTerm term = {
          .kind = EFFECT_TERM_NAMED,
          .expr = NULL,
          .decl = h->op_perform.effect_decl,
          .name_id = h->op_perform.effect_decl->name.string_id,
      };
      effect_set_add(set, term);
    }
    if (h->op_perform.args) {
      for (size_t i = 0; i < h->op_perform.args->count; i++) {
        struct HirInstr **ap =
            (struct HirInstr **)vec_get(h->op_perform.args, i);
        if (ap && *ap)
          collect_from_hir(s, set, *ap);
      }
    }
    return;

  case HIR_HANDLER_INSTALL: {
    // Walk the handler's own sub-expressions for incidental
    // effects (rare — e.g. lifecycle clauses calling fns).
    struct HirInstr *handler = h->handler_install.handler;
    if (handler && handler->kind == HIR_HANDLER_VALUE) {
      struct HirHandlerValuePayload *hv = &handler->handler_value;
      if (hv->operations) {
        for (size_t i = 0; i < hv->operations->count; i++) {
          struct HirHandlerOp **opp =
              (struct HirHandlerOp **)vec_get(hv->operations, i);
          if (opp && *opp && (*opp)->body_block) {
            collect_from_hir_block(s, set, (*opp)->body_block);
          }
        }
      }
      if (hv->initially_block)
        collect_from_hir_block(s, set, hv->initially_block);
      if (hv->finally_block)
        collect_from_hir_block(s, set, hv->finally_block);
      if (hv->return_block)
        collect_from_hir_block(s, set, hv->return_block);
    }
    // Body effects, then subtract the handled effect.
    struct EffectSet *body_set = effect_set_new(s);
    collect_from_hir_block(s, body_set, h->handler_install.body_block);
    struct Decl *handled = h->handler_install.effect_decl;
    if (handled && !effect_set_contains_decl(body_set, handled) &&
        !body_set->open) {
      const char *nm = pool_get(s->pool, handled->name.string_id, 0);
      if (s->diags) {
        diag_add(
            s->diags, DIAG_WARNING, h->span,
            "handler discharges effect '%s' but the body never performs it",
            nm ? nm : "?");
      }
    }
    effect_set_copy_minus(set, body_set, handled);
    return;
  }

  case HIR_HANDLER_VALUE:
    // A handler literal as a value — its op bodies belong to it,
    // not the surrounding fn. Don't recurse.
    return;

  case HIR_LAMBDA:
    // Lambda body effects belong to the inner HirFn (verified
    // when its enclosing decl gets sema_verify_body_effects).
    // Don't double-count.
    return;

  case HIR_BIN:
    collect_from_hir(s, set, h->bin.left);
    collect_from_hir(s, set, h->bin.right);
    return;
  case HIR_UNARY:
    collect_from_hir(s, set, h->unary.operand);
    return;
  case HIR_ASSIGN:
    collect_from_hir(s, set, h->assign.target);
    collect_from_hir(s, set, h->assign.value);
    return;
  case HIR_FIELD:
    collect_from_hir(s, set, h->field.object);
    return;
  case HIR_INDEX:
    collect_from_hir(s, set, h->index.object);
    collect_from_hir(s, set, h->index.index);
    return;
  case HIR_IF:
    collect_from_hir(s, set, h->if_instr.condition);
    collect_from_hir_block(s, set, h->if_instr.then_block);
    collect_from_hir_block(s, set, h->if_instr.else_block);
    return;
  case HIR_LOOP:
    collect_from_hir(s, set, h->loop.init);
    collect_from_hir(s, set, h->loop.condition);
    collect_from_hir(s, set, h->loop.step);
    collect_from_hir_block(s, set, h->loop.body_block);
    return;
  case HIR_SWITCH:
    collect_from_hir(s, set, h->switch_instr.scrutinee);
    if (h->switch_instr.arms) {
      for (size_t i = 0; i < h->switch_instr.arms->count; i++) {
        struct HirSwitchArm **ap =
            (struct HirSwitchArm **)vec_get(h->switch_instr.arms, i);
        if (!ap || !*ap)
          continue;
        if ((*ap)->patterns) {
          for (size_t j = 0; j < (*ap)->patterns->count; j++) {
            struct HirInstr **pp =
                (struct HirInstr **)vec_get((*ap)->patterns, j);
            if (pp && *pp)
              collect_from_hir(s, set, *pp);
          }
        }
        collect_from_hir_block(s, set, (*ap)->body_block);
      }
    }
    return;
  case HIR_BIND:
    collect_from_hir(s, set, h->bind.init);
    return;
  case HIR_RETURN:
    collect_from_hir(s, set, h->return_instr.value);
    return;
  case HIR_DEFER:
    collect_from_hir(s, set, h->defer.value);
    return;
  case HIR_PRODUCT:
    if (h->product.fields) {
      for (size_t i = 0; i < h->product.fields->count; i++) {
        struct HirInstr **fp =
            (struct HirInstr **)vec_get(h->product.fields, i);
        if (fp && *fp)
          collect_from_hir(s, set, *fp);
      }
    }
    return;
  case HIR_ARRAY_LIT:
    collect_from_hir(s, set, h->array_lit.size);
    collect_from_hir(s, set, h->array_lit.initializer);
    return;
  case HIR_BUILTIN:
    // Builtins evaluate at compile time today. If a builtin
    // ever performs a runtime effect, this is where we'd
    // dispatch on name_id; for now, walk args for incidental
    // sub-effects only.
    if (h->builtin.args) {
      for (size_t i = 0; i < h->builtin.args->count; i++) {
        struct HirInstr **ap = (struct HirInstr **)vec_get(h->builtin.args, i);
        if (ap && *ap)
          collect_from_hir(s, set, *ap);
      }
    }
    return;
  }
}

// ----- Public queries -----

struct EffectSig *sema_effect_sig_of_callable(struct Sema *s,
                                              struct Decl *decl) {
  if (!s || !decl)
    return NULL;
  struct SemaDeclInfo *info = sema_decl_info(s, decl);
  if (!info)
    return NULL;
  QueryBeginResult begin = sema_query_begin(
      s, &info->effect_sig_query, QUERY_EFFECT_SIG, decl, decl->name.span);
  if (begin == QUERY_BEGIN_CACHED || begin == QUERY_BEGIN_CYCLE ||
      begin == QUERY_BEGIN_ERROR) {
    return info->effect_sig;
  }
  // Signature resolution already populated info->effect_sig as a side effect
  // of sema_signature_of_decl. This query is the official cache for it.
  //
  // No fail path: a NULL `info->effect_sig` means "no declared effects",
  // not "error". Callers can't distinguish "pure function" from "failed
  // analysis" via this API and don't need to — diagnostics for malformed
  // effect annotations are emitted upstream.
  sema_query_succeed(s, &info->effect_sig_query);
  return info->effect_sig;
}

struct EffectSet *sema_body_effects_of(struct Sema *s, struct Decl *decl) {
  if (!s || !decl)
    return NULL;
  struct SemaDeclInfo *info = sema_decl_info(s, decl);
  if (!info)
    return NULL;
  QueryBeginResult begin = sema_query_begin(
      s, &info->body_effects_query, QUERY_BODY_EFFECTS, decl, decl->name.span);
  if (begin == QUERY_BEGIN_CACHED)
    return info->body_effects;
  if (begin == QUERY_BEGIN_ERROR || begin == QUERY_BEGIN_CYCLE)
    return NULL;

  // Walk the per-decl HIR built by sema_lower_modules. Phase G.3
  // deleted the AST-walker fallback — sema_body_effects_of is now
  // only sound after lowering completes. The verify-effects post-pass
  // and any future codegen consumer call it from that point onward.
  struct EffectSet *set = effect_set_new(s);
  struct HirFn *fn =
      (struct HirFn *)hashmap_get(&s->decl_hir, (uint64_t)(uintptr_t)decl);
  if (fn)
    collect_from_hir_block(s, set, fn->body_block);
  info->body_effects = set;
  // No fail path: an empty `body_effects` means "this body performs no
  // effects" — a valid result, not an error. Per-call mismatches between
  // body effects and declared sig are diagnosed by `sema_solve_effect_rows`
  // (which the type-checker calls separately), not by failing this query.
  sema_query_succeed(s, &info->body_effects_query);
  return set;
}

bool sema_solve_effect_rows(struct Sema *s, struct Decl *decl,
                            struct EffectSig *declared,
                            struct EffectSet *inferred) {
  if (!s || !inferred)
    return true;
  if (!inferred->terms || inferred->terms->count == 0)
    return true;

  bool ok = true;
  for (size_t i = 0; i < inferred->terms->count; i++) {
    struct EffectTerm *term = (struct EffectTerm *)vec_get(inferred->terms, i);
    if (!term)
      continue;
    if (term->kind == EFFECT_TERM_UNKNOWN)
      continue;

    bool covered = false;
    if (declared && declared->terms) {
      for (size_t j = 0; j < declared->terms->count; j++) {
        struct EffectTerm *d = (struct EffectTerm *)vec_get(declared->terms, j);
        if (effect_term_matches(d, term)) {
          covered = true;
          break;
        }
      }
    }
    // Open row absorbs anything not explicitly listed.
    if (!covered && declared && declared->is_open)
      covered = true;

    if (!covered) {
      const char *fn_name =
          (s->pool && decl) ? pool_get(s->pool, decl->name.string_id, 0) : NULL;
      const char *eff_name =
          s->pool && term->name_id ? pool_get(s->pool, term->name_id, 0) : NULL;
      sema_error(s, decl ? decl->name.span : (struct Span){0},
                 "function '%s' performs effect '%s' but its signature does "
                 "not declare it",
                 fn_name ? fn_name : "?", eff_name ? eff_name : "?");
      ok = false;
    }
  }
  return ok;
}

// ----- Verification post-pass -----
//
// Phase E moved per-decl body-effects verification out of
// `compute_decl_signature` into this dedicated post-pass. Bidirectional
// type-checking determines what the body actually performs; this pass
// then walks each function decl, infers its effect set from the body,
// and verifies it matches the declared signature row. Mirrors how
// Effekt-style languages stage effect checking against explicit
// annotations (the inferred set must be a subset of the declared row,
// modulo row-variable solving).
//
// Walks per-decl HIR built by sema_lower_modules and per-instantiation
// HIR built alongside it. Both paths converge in collect_from_hir_block.

static bool decl_function_params(struct Decl *decl, Vec **out) {
  if (!decl || !decl->node)
    return false;
  if (decl->node->kind == expr_Lambda) {
    *out = decl->node->lambda.params;
    return true;
  }
  if (decl->node->kind == expr_Ctl) {
    *out = decl->node->ctl.params;
    return true;
  }
  if (decl->node->kind == expr_Bind && decl->node->bind.value) {
    struct Expr *v = decl->node->bind.value;
    if (v->kind == expr_Lambda) {
      *out = v->lambda.params;
      return true;
    }
    if (v->kind == expr_Ctl) {
      *out = v->ctl.params;
      return true;
    }
  }
  return false;
}

static bool decl_is_generic(struct Decl *decl) {
  Vec *params = NULL;
  if (!decl_function_params(decl, &params) || !params)
    return false;
  for (size_t i = 0; i < params->count; i++) {
    struct Param *p = (struct Param *)vec_get(params, i);
    if (p && p->kind != PARAM_RUNTIME)
      return true;
  }
  return false;
}

static void verify_decl(struct Sema *s, struct Decl *decl) {
  if (!decl)
    return;
  if (decl->semantic_kind != SEM_VALUE)
    return;
  struct SemaDeclInfo *info = sema_decl_info(s, decl);
  if (!info || !info->type || info->type->kind != TYPE_FUNCTION)
    return;
  if (decl_is_generic(decl))
    return; // per-instantiation check covers these
  // Skip handler-op implementations (their declared sig comes from the
  // effect, not the surrounding function — body may perform any effect
  // of the enclosing scope).
  if (s->compiler && hashmap_contains(&s->compiler->handler_impl_decls,
                                      (uint64_t)(uintptr_t)decl)) {
    return;
  }
  struct EffectSet *inferred = sema_body_effects_of(s, decl);
  if (inferred)
    sema_solve_effect_rows(s, decl, info->effect_sig, inferred);
}

static void verify_scope(struct Sema *s, struct Scope *scope) {
  if (!scope || !scope->decls)
    return;
  for (size_t i = 0; i < scope->decls->count; i++) {
    struct Decl **dp = (struct Decl **)vec_get(scope->decls, i);
    struct Decl *d = dp ? *dp : NULL;
    if (d)
      verify_decl(s, d);
  }
}

// Per-instantiation verification (Phase G.2). Each generic
// specialization may perform a different effect set than the generic
// (different comptime args produce structurally different bodies).
// Walk inst->hir->body_block via collect_from_hir_block; compare
// against inst->specialized_sig (or the generic's sig as fallback).
static void verify_instantiation(struct Sema *s, struct Instantiation *inst) {
  if (!s || !inst || !inst->generic || !inst->hir)
    return;
  if (s->compiler && hashmap_contains(&s->compiler->handler_impl_decls,
                                      (uint64_t)(uintptr_t)inst->generic)) {
    return;
  }
  struct EffectSet *inferred = effect_set_new(s);
  collect_from_hir_block(s, inferred, inst->hir->body_block);
  struct EffectSig *declared = inst->specialized_sig
                                   ? inst->specialized_sig
                                   : sema_decl_effect_sig(s, inst->generic);
  sema_solve_effect_rows(s, inst->generic, declared, inferred);
}

void sema_verify_body_effects(struct Sema *s) {
  if (!s || !s->compiler || !s->compiler->modules)
    return;
  Vec *modules = s->compiler->modules;
  for (size_t i = 0; i < modules->count; i++) {
    struct Module **mp = (struct Module **)vec_get(modules, i);
    struct Module *mod = mp ? *mp : NULL;
    if (mod && mod->scope)
      verify_scope(s, mod->scope);
  }
  if (s->instantiations) {
    for (size_t i = 0; i < s->instantiations->count; i++) {
      struct Instantiation **ip =
          (struct Instantiation **)vec_get(s->instantiations, i);
      struct Instantiation *inst = ip ? *ip : NULL;
      if (inst)
        verify_instantiation(s, inst);
    }
  }
}
