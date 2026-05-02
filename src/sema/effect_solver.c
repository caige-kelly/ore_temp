#include "effect_solver.h"

#include "effects.h"
#include "evidence.h"
#include "instantiate.h"
#include "sema.h"
#include "sema_internal.h"
#include "type.h"
#include "../diag/diag.h"
#include "../parser/ast.h"

// ----- EffectSet helpers -----

static struct EffectSet* effect_set_new(struct Sema* s) {
    if (!s || !s->arena) return NULL;
    struct EffectSet* set = arena_alloc(s->arena, sizeof(struct EffectSet));
    if (!set) return NULL;
    set->terms = vec_new_in(s->arena, sizeof(struct EffectTerm));
    set->open = false;
    set->open_row_name_id = 0;
    return set;
}

static bool effect_term_matches(const struct EffectTerm* a, const struct EffectTerm* b) {
    if (!a || !b) return false;
    if (a->decl && b->decl) return a->decl == b->decl;
    if (a->name_id && b->name_id) return a->name_id == b->name_id;
    return false;
}

// TODO(perf): linear scan, called per add. Fine while typical sets are < 30
// terms; revisit with a per-set hashset keyed by (decl, scope_token_id) once
// real programs push us past that. Do not bother before then.
static bool effect_set_contains(struct EffectSet* set, const struct EffectTerm* term) {
    if (!set || !set->terms || !term) return false;
    for (size_t i = 0; i < set->terms->count; i++) {
        struct EffectTerm* t = (struct EffectTerm*)vec_get(set->terms, i);
        if (effect_term_matches(t, term)) return true;
    }
    return false;
}

static void effect_set_add(struct EffectSet* set, struct EffectTerm term) {
    if (!set || !set->terms) return;
    if (term.kind == EFFECT_TERM_UNKNOWN) return;
    if (effect_set_contains(set, &term)) return;
    vec_push(set->terms, &term);
}

// Copy every term from `src` into `dst` *except* those that match
// `discharged`. Open-row state copies through unchanged. The standard
// dedup in `effect_set_add` handles overlap with terms already in `dst`.
//
// This is the discharge primitive for handlers: a handler that
// discharges effect E walks its body's effects, drops the E term, and
// contributes the rest to the surrounding scope's set.
static void effect_set_copy_minus(struct EffectSet* dst,
                                   struct EffectSet* src,
                                   struct Decl* discharged) {
    if (!dst || !src) return;
    if (src->terms) {
        for (size_t i = 0; i < src->terms->count; i++) {
            struct EffectTerm* t = (struct EffectTerm*)vec_get(src->terms, i);
            if (!t) continue;
            if (discharged && t->decl == discharged) continue;  // dropped
            effect_set_add(dst, *t);
        }
    }
    if (src->open) {
        dst->open = true;
        if (dst->open_row_name_id == 0) dst->open_row_name_id = src->open_row_name_id;
    }
}

// True if `set` contains a term whose effect is `eff`. Used by handler
// discharge to detect dead handlers (the handler discharges an effect
// the body never performs).
static bool effect_set_contains_decl(struct EffectSet* set, struct Decl* eff) {
    if (!set || !set->terms || !eff) return false;
    for (size_t i = 0; i < set->terms->count; i++) {
        struct EffectTerm* t = (struct EffectTerm*)vec_get(set->terms, i);
        if (t && t->decl == eff) return true;
    }
    return false;
}

// Climb from an effect-op decl (a FIELD living in a SCOPE_EFFECT scope) back
// to the Decl of the effect itself.
static struct Decl* effect_decl_for_op(struct Decl* op) {
    if (!op || !op->owner) return NULL;
    if (op->owner->kind != SCOPE_EFFECT) return NULL;
    struct Scope* eff_scope = op->owner;
    struct Scope* parent = eff_scope->parent;
    if (!parent || !parent->decls) return NULL;
    for (size_t i = 0; i < parent->decls->count; i++) {
        struct Decl** d_p = (struct Decl**)vec_get(parent->decls, i);
        struct Decl* d = d_p ? *d_p : NULL;
        if (d && d->child_scope == eff_scope && d->semantic_kind == SEM_EFFECT) return d;
    }
    return NULL;
}

// ----- Body walk -----

static void collect_from_expr(struct Sema* s, struct EffectSet* set, struct Expr* expr);

static void collect_from_vec(struct Sema* s, struct EffectSet* set, Vec* exprs) {
    if (!exprs) return;
    for (size_t i = 0; i < exprs->count; i++) {
        struct Expr** e_p = (struct Expr**)vec_get(exprs, i);
        if (e_p && *e_p) collect_from_expr(s, set, *e_p);
    }
}

static void union_callee_effects(struct EffectSet* set, struct EffectSig* sig) {
    if (!set || !sig) return;
    if (sig->is_open) {
        set->open = true;
        if (set->open_row_name_id == 0) set->open_row_name_id = sig->row_name_id;
    }
    if (!sig->terms) return;
    for (size_t i = 0; i < sig->terms->count; i++) {
        struct EffectTerm* t = (struct EffectTerm*)vec_get(sig->terms, i);
        if (t) effect_set_add(set, *t);
    }
}

// Subtract every term in `discharge` from `src`, adding the rest to
// `dst`. Open-row state copies through. Used by signature-driven
// discharge to drop the action's declared effects from a body lambda's
// inferred effects.
static void effect_set_copy_minus_set(struct EffectSet* dst,
                                       struct EffectSet* src,
                                       struct EffectSig* discharge) {
    if (!dst || !src) return;
    if (src->terms) {
        for (size_t i = 0; i < src->terms->count; i++) {
            struct EffectTerm* t = (struct EffectTerm*)vec_get(src->terms, i);
            if (!t) continue;
            bool drop = false;
            if (discharge && discharge->terms) {
                for (size_t j = 0; j < discharge->terms->count; j++) {
                    struct EffectTerm* d = (struct EffectTerm*)vec_get(discharge->terms, j);
                    if (effect_term_matches(d, t)) { drop = true; break; }
                }
            }
            if (!drop) effect_set_add(dst, *t);
        }
    }
    if (src->open) {
        dst->open = true;
        if (dst->open_row_name_id == 0) dst->open_row_name_id = src->open_row_name_id;
    }
}

static void collect_from_call(struct Sema* s, struct EffectSet* set, struct Expr* call) {
    struct Expr* callee = call->call.callee;

    // Phase 6.1: handler-literal callee (`with handler {ops} body` after
    // desugar = `Call(<expr_Handler>, [Lambda(body)])`). Treat the call
    // as a discharge site: subtract the handler's effect from the body
    // lambda's effects.
    if (callee && callee->kind == expr_Handler &&
        call->call.args && call->call.args->count == 1) {
        struct Expr** arg0p = (struct Expr**)vec_get(call->call.args, 0);
        struct Expr* arg0 = arg0p ? *arg0p : NULL;
        if (arg0 && arg0->kind == expr_Lambda) {
            collect_from_expr(s, set, callee);
            struct Decl* handled = callee->handler.effect_decl;
            struct EffectSet* body_set = effect_set_new(s);
            collect_from_expr(s, body_set, arg0->lambda.body);
            // Phase 6.3: dead-handler warning. If the handler claims to
            // discharge an effect the body never performs, the handler
            // is doing no work — flag it so the user removes it (or
            // notices a missing op call).
            if (handled && !effect_set_contains_decl(body_set, handled) && !body_set->open) {
                const char* nm = pool_get(s->pool, handled->name.string_id, 0);
                if (s->diags) {
                    diag_add(s->diags, DIAG_WARNING, callee->span,
                        "handler discharges effect '%s' but the body never performs it",
                        nm ? nm : "?");
                }
            }
            effect_set_copy_minus(set, body_set, handled);
            return;
        }
    }

    if (callee) collect_from_expr(s, set, callee);

    // Get callee's declared signature once for both arg-walking and the
    // outer effect contribution below.
    struct Decl* callee_decl = ast_resolved_decl_of(callee);
    struct Type* ctype = callee_decl ? sema_decl_type(s, callee_decl) : NULL;

    // Phase 6.2: signature-driven discharge for action params.
    //
    // For every runtime arg whose corresponding param type is a function
    // type with a declared effect signature (an "action" the callee will
    // run), walk the arg lambda's body, subtract the action's declared
    // effects, and add the residual to `set`. The non-discharged residual
    // is what leaks back out — catches soundness gaps where the body
    // performs effects beyond what the action's annotation declared.
    //
    // Iterate the AST Param vec (which carries `kind` so we can skip
    // PARAM_INFERRED_COMPTIME slots) in lockstep with the call's args
    // and the type-level params vec.
    Vec* ast_params = callee_decl ? sema_decl_function_params(callee_decl) : NULL;
    Vec* type_params = ctype ? ctype->params : NULL;
    bool walked_args = false;
    if (ast_params && call->call.args) {
        size_t arg_i = 0;
        for (size_t pi = 0; pi < ast_params->count; pi++) {
            struct Param* p = (struct Param*)vec_get(ast_params, pi);
            if (!p) continue;
            if (p->kind == PARAM_INFERRED_COMPTIME) continue;  // sema fills, no arg
            if (arg_i >= call->call.args->count) break;
            struct Expr** ap = (struct Expr**)vec_get(call->call.args, arg_i);
            struct Expr* arg = ap ? *ap : NULL;
            struct Type* param_ty = NULL;
            if (type_params && pi < type_params->count) {
                struct Type** tp = (struct Type**)vec_get(type_params, pi);
                param_ty = tp ? *tp : NULL;
            }
            arg_i++;
            if (!arg) continue;
            bool action_shape = arg->kind == expr_Lambda &&
                param_ty && param_ty->kind == TYPE_FUNCTION &&
                param_ty->effect_sig;
            if (action_shape) {
                struct EffectSet* body_set = effect_set_new(s);
                collect_from_expr(s, body_set, arg->lambda.body);
                effect_set_copy_minus_set(set, body_set, param_ty->effect_sig);
            } else {
                collect_from_expr(s, set, arg);
            }
        }
        walked_args = true;
    }
    if (!walked_args && call->call.args) {
        // Fallback: no callee-decl info available — walk args normally.
        // Discharge can't fire here, but soundness is unaffected because
        // we haven't claimed to discharge anything.
        collect_from_vec(s, set, call->call.args);
    }

    if (callee_decl) {
        // 1) Callee's own declared effects flow up.
        struct EffectSig* esig = sema_decl_effect_sig(s, callee_decl);
        if (esig) union_callee_effects(set, esig);
        else if (ctype && ctype->effect_sig) {
            union_callee_effects(set, ctype->effect_sig);
        }

        // 2) Calling an op of an effect implicitly performs that effect.
        struct Decl* eff = effect_decl_for_op(callee_decl);
        if (eff) {
            struct EffectTerm term = {
                .kind = EFFECT_TERM_NAMED,
                .expr = call,
                .decl = eff,
                .name_id = eff->name.string_id,
            };
            effect_set_add(set, term);
        }
    }
}

static void collect_from_block(struct Sema* s, struct EffectSet* set, Vec* stmts) {
    if (!stmts) return;
    for (size_t i = 0; i < stmts->count; i++) {
        struct Expr** e_p = (struct Expr**)vec_get(stmts, i);
        if (e_p && *e_p) collect_from_expr(s, set, *e_p);
    }
}

// Walk a body expression and union every effect it performs into `set`.
//
// **Intentional subset**: this walker only enters expression kinds that
// can transitively reach a Call (the only construct that performs an
// effect today). Type-shape nodes (Lambda, Ctl, Struct, Enum, Effect,
// EffectRow, ArrayType, SliceType, ManyPtrType), pattern nodes (EnumRef,
// Wildcard, DestructureBind), pure leaves (Lit, Asm, Break, Continue),
// and Builtin / Product / ArrayLit / Bind/Type-annotation paths either
// can't perform effects or have their effects accounted for elsewhere
// (e.g. nested function bodies live in their own EffectSet).
//
// `default: return;` is therefore correct for missed kinds — a new kind
// is not assumed to perform effects until explicitly handled here.
static void collect_from_expr(struct Sema* s, struct EffectSet* set, struct Expr* expr) {
    if (!expr) return;

    switch (expr->kind) {
        case expr_Call:
            collect_from_call(s, set, expr);
            return;
        case expr_Block:
            collect_from_block(s, set, expr->block.stmts);
            return;
        case expr_If:
            collect_from_expr(s, set, expr->if_expr.condition);
            collect_from_expr(s, set, expr->if_expr.then_branch);
            collect_from_expr(s, set, expr->if_expr.else_branch);
            return;
        case expr_Bin:
            collect_from_expr(s, set, expr->bin.Left);
            collect_from_expr(s, set, expr->bin.Right);
            return;
        case expr_Assign:
            collect_from_expr(s, set, expr->assign.target);
            collect_from_expr(s, set, expr->assign.value);
            return;
        case expr_Unary:
            collect_from_expr(s, set, expr->unary.operand);
            return;
        case expr_Field:
            collect_from_expr(s, set, expr->field.object);
            return;
        case expr_Index:
            collect_from_expr(s, set, expr->index.object);
            collect_from_expr(s, set, expr->index.index);
            return;
        case expr_Loop:
            collect_from_expr(s, set, expr->loop_expr.init);
            collect_from_expr(s, set, expr->loop_expr.condition);
            collect_from_expr(s, set, expr->loop_expr.step);
            collect_from_expr(s, set, expr->loop_expr.body);
            return;
        case expr_Bind:
            collect_from_expr(s, set, expr->bind.value);
            return;
        case expr_Return:
            collect_from_expr(s, set, expr->return_expr.value);
            return;
        case expr_Defer:
            collect_from_expr(s, set, expr->defer_expr.value);
            return;
        case expr_NamedBind: {
            // `with s := named target body` discharges target's effect
            // E from the body's effect set. Phase 5 typed `target` as
            // HandlerOf<E, R>; Phase 3 stashed E on `Type.decl`. Walk
            // target's own effects (non-discharging — they leak up),
            // collect the body's effects into a temp set, and copy
            // everything except E into the outer set.
            collect_from_expr(s, set, expr->named_bind.target);
            struct Decl* handled = NULL;
            // Prefer the AST-attached effect when target is an
            // expr_Handler literal (resolver fills it in Phase 2);
            // fall back to the typed view (HandlerOf<E,R>'s E lives
            // on Type.decl) for arbitrary handler-returning targets.
            if (expr->named_bind.target &&
                expr->named_bind.target->kind == expr_Handler) {
                handled = expr->named_bind.target->handler.effect_decl;
            } else {
                struct Type* target_ty = sema_type_of(s, expr->named_bind.target);
                if (target_ty && target_ty->kind == TYPE_HANDLER) {
                    handled = target_ty->decl;
                }
            }
            struct EffectSet* body_set = effect_set_new(s);
            collect_from_expr(s, body_set, expr->named_bind.body);
            // Phase 6.3: dead-handler warning. Mirrors the handler-
            // literal case above. Open-row bodies are exempt — the row
            // variable could legitimately bind to a set containing the
            // handled effect at instantiation time.
            if (handled && !effect_set_contains_decl(body_set, handled) && !body_set->open) {
                const char* nm = pool_get(s->pool, handled->name.string_id, 0);
                if (s->diags) {
                    diag_add(s->diags, DIAG_WARNING, expr->span,
                        "named handler discharges effect '%s' but the body never performs it",
                        nm ? nm : "?");
                }
            }
            effect_set_copy_minus(set, body_set, handled);
            return;
        }
        case expr_Switch:
            collect_from_expr(s, set, expr->switch_expr.scrutinee);
            if (expr->switch_expr.arms) {
                for (size_t i = 0; i < expr->switch_expr.arms->count; i++) {
                    struct SwitchArm* arm = (struct SwitchArm*)vec_get(expr->switch_expr.arms, i);
                    if (!arm) continue;
                    if (arm->patterns) {
                        for (size_t j = 0; j < arm->patterns->count; j++) {
                            struct Expr** p_p = (struct Expr**)vec_get(arm->patterns, j);
                            if (p_p && *p_p) collect_from_expr(s, set, *p_p);
                        }
                    }
                    collect_from_expr(s, set, arm->body);
                }
            }
            return;
        default:
            return;
    }
}

// ----- Public queries -----

struct EffectSig* sema_effect_sig_of_callable(struct Sema* s, struct Decl* decl) {
    if (!s || !decl) return NULL;
    struct SemaDeclInfo* info = sema_decl_info(s, decl);
    if (!info) return NULL;
    QueryBeginResult begin = sema_query_begin(s, &info->effect_sig_query,
        QUERY_EFFECT_SIG, decl, decl->name.span);
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

struct EffectSet* sema_collect_effects_from_expr(struct Sema* s, struct Expr* expr) {
    if (!s) return NULL;
    struct EffectSet* set = effect_set_new(s);
    if (expr) collect_from_expr(s, set, expr);
    return set;
}

struct EffectSet* sema_body_effects_of(struct Sema* s, struct Decl* decl) {
    if (!s || !decl) return NULL;
    struct SemaDeclInfo* info = sema_decl_info(s, decl);
    if (!info) return NULL;
    QueryBeginResult begin = sema_query_begin(s, &info->body_effects_query,
        QUERY_BODY_EFFECTS, decl, decl->name.span);
    if (begin == QUERY_BEGIN_CACHED) return info->body_effects;
    if (begin == QUERY_BEGIN_ERROR || begin == QUERY_BEGIN_CYCLE) return NULL;

    struct Expr* body = sema_decl_function_body(decl);
    struct EffectSet* set = effect_set_new(s);
    if (body) collect_from_expr(s, set, body);
    info->body_effects = set;
    // No fail path: an empty `body_effects` means "this body performs no
    // effects" — a valid result, not an error. Per-call mismatches between
    // body effects and declared sig are diagnosed by `sema_solve_effect_rows`
    // (which the type-checker calls separately), not by failing this query.
    sema_query_succeed(s, &info->body_effects_query);
    return set;
}

bool sema_solve_effect_rows(struct Sema* s, struct Decl* decl,
    struct EffectSig* declared, struct EffectSet* inferred) {
    if (!s || !inferred) return true;
    if (!inferred->terms || inferred->terms->count == 0) return true;

    bool ok = true;
    for (size_t i = 0; i < inferred->terms->count; i++) {
        struct EffectTerm* term = (struct EffectTerm*)vec_get(inferred->terms, i);
        if (!term) continue;
        if (term->kind == EFFECT_TERM_UNKNOWN) continue;

        bool covered = false;
        if (declared && declared->terms) {
            for (size_t j = 0; j < declared->terms->count; j++) {
                struct EffectTerm* d = (struct EffectTerm*)vec_get(declared->terms, j);
                if (effect_term_matches(d, term)) { covered = true; break; }
            }
        }
        // Open row absorbs anything not explicitly listed.
        if (!covered && declared && declared->is_open) covered = true;

        if (!covered) {
            const char* fn_name = (s->pool && decl)
                ? pool_get(s->pool, decl->name.string_id, 0) : NULL;
            const char* eff_name = s->pool && term->name_id
                ? pool_get(s->pool, term->name_id, 0) : NULL;
            sema_error(s, decl ? decl->name.span : (struct Span){0},
                "function '%s' performs effect '%s' but its signature does not declare it",
                fn_name ? fn_name : "?",
                eff_name ? eff_name : "?");
            ok = false;
        }
    }
    return ok;
}
