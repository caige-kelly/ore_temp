#include "effect_solver.h"

#include "effects.h"
#include "evidence.h"
#include "instantiate.h"
#include "sema.h"
#include "sema_internal.h"
#include "type.h"
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

static void collect_from_call(struct Sema* s, struct EffectSet* set, struct Expr* call) {
    struct Expr* callee = call->call.callee;
    if (callee) collect_from_expr(s, set, callee);
    if (call->call.args) collect_from_vec(s, set, call->call.args);

    // 1) Callee with an effect signature: union its declared terms.
    struct Decl* callee_decl = NULL;
    if (callee && callee->kind == expr_Ident) callee_decl = callee->ident.resolved;
    else if (callee && callee->kind == expr_Field) callee_decl = callee->field.field.resolved;

    if (callee_decl) {
        struct EffectSig* esig = sema_decl_effect_sig(s, callee_decl);
        struct Type* ctype = sema_decl_type(s, callee_decl);
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

static void collect_from_expr(struct Sema* s, struct EffectSet* set, struct Expr* expr) {
    if (!expr) return;

    switch (expr->kind) {
        case expr_Call:
            collect_from_call(s, set, expr);
            return;
        case expr_Block:
            collect_from_block(s, set, &expr->block.stmts);
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
        case expr_With: {
            // `with H { body }` discharges *only* H's effects from `body`. Any
            // other effects performed inside the body still leak upward.
            // Resolve H via the same rules name-resolution uses, then collect
            // the body's effects into a temp set, copy across everything that
            // doesn't match H.
            collect_from_expr(s, set, expr->with.func);

            struct Decl* handled = expr->with.handled_effect;
            struct EffectSet* body_set = effect_set_new(s);
            collect_from_expr(s, body_set, expr->with.body);

            if (body_set && body_set->terms) {
                for (size_t i = 0; i < body_set->terms->count; i++) {
                    struct EffectTerm* t = (struct EffectTerm*)vec_get(body_set->terms, i);
                    if (!t) continue;
                    if (handled && t->decl == handled) continue; // discharged by this `with`
                    effect_set_add(set, *t);
                }
                if (body_set->open) {
                    set->open = true;
                    if (set->open_row_name_id == 0) set->open_row_name_id = body_set->open_row_name_id;
                }
            }
            return;
        }
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
