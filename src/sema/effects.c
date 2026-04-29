#include "effects.h"

#include <stdio.h>

#include "sema.h"

static struct EffectSig* effect_sig_find_existing(struct Sema* s, struct Expr* source) {
    if (!s || !source || !s->effect_sigs) return NULL;
    for (size_t i = 0; i < s->effect_sigs->count; i++) {
        struct EffectSig** sig_p = (struct EffectSig**)vec_get(s->effect_sigs, i);
        if (sig_p && *sig_p && (*sig_p)->source == source) return *sig_p;
    }
    return NULL;
}

static struct EffectSig* effect_sig_new(struct Sema* s, struct Expr* source) {
    struct EffectSig* sig = arena_alloc(s->arena, sizeof(struct EffectSig));
    if (!sig) return NULL;
    sig->source = source;
    sig->terms = vec_new_in(s->arena, sizeof(struct EffectTerm));
    return sig;
}

static struct Decl* resolved_decl_from_effect_expr(struct Expr* expr) {
    if (!expr) return NULL;
    switch (expr->kind) {
        case expr_Ident: return expr->ident.resolved;
        case expr_Field: return expr->field.field.resolved;
        default: return NULL;
    }
}

static uint32_t name_id_from_effect_expr(struct Expr* expr) {
    if (!expr) return 0;
    switch (expr->kind) {
        case expr_Ident: return expr->ident.string_id;
        case expr_Field: return expr->field.field.string_id;
        default: return 0;
    }
}

static uint32_t scope_token_id_from_args(Vec* args) {
    if (!args) return 0;
    for (size_t i = 0; i < args->count; i++) {
        struct Expr** arg_p = (struct Expr**)vec_get(args, i);
        struct Expr* arg = arg_p ? *arg_p : NULL;
        if (!arg || arg->kind != expr_Ident || !arg->ident.resolved) continue;
        if (arg->ident.resolved->semantic_kind == SEM_SCOPE_TOKEN) {
            return arg->ident.resolved->scope_token_id;
        }
    }
    return 0;
}

static void effect_sig_push_term(struct EffectSig* sig, struct EffectTerm term) {
    if (!sig || !sig->terms) return;
    vec_push(sig->terms, &term);
}

static void effect_sig_push_row(struct EffectSig* sig, struct Identifier row) {
    if (!sig || row.string_id == 0) return;
    sig->is_open = true;
    sig->row_name_id = row.string_id;
    sig->row_decl = row.resolved;
    struct EffectTerm term = {
        .kind = EFFECT_TERM_ROW,
        .decl = row.resolved,
        .row_name_id = row.string_id,
    };
    effect_sig_push_term(sig, term);
}

static void effect_sig_collect_term(struct EffectSig* sig, struct Expr* expr) {
    if (!expr) return;

    switch (expr->kind) {
        case expr_EffectRow:
            effect_sig_collect_term(sig, expr->effect_row.head);
            effect_sig_push_row(sig, expr->effect_row.row);
            return;
        case expr_Bin:
            if (expr->bin.op == Pipe) {
                effect_sig_collect_term(sig, expr->bin.Left);
                effect_sig_collect_term(sig, expr->bin.Right);
                return;
            }
            break;
        case expr_Call: {
            struct Expr* callee = expr->call.callee;
            struct Decl* decl = resolved_decl_from_effect_expr(callee);
            uint32_t scope_token_id = scope_token_id_from_args(expr->call.args);
            struct EffectTerm term = {
                .kind = scope_token_id ? EFFECT_TERM_SCOPED : EFFECT_TERM_NAMED,
                .expr = expr,
                .decl = decl,
                .name_id = name_id_from_effect_expr(callee),
                .scope_token_id = scope_token_id,
            };
            if (decl && decl->semantic_kind != SEM_EFFECT) term.kind = EFFECT_TERM_UNKNOWN;
            effect_sig_push_term(sig, term);
            return;
        }
        case expr_Ident:
        case expr_Field: {
            struct Decl* decl = resolved_decl_from_effect_expr(expr);
            struct EffectTerm term = {
                .kind = EFFECT_TERM_NAMED,
                .expr = expr,
                .decl = decl,
                .name_id = name_id_from_effect_expr(expr),
            };
            if (decl && decl->semantic_kind != SEM_EFFECT) term.kind = EFFECT_TERM_UNKNOWN;
            effect_sig_push_term(sig, term);
            return;
        }
        default:
            break;
    }

    struct EffectTerm term = {
        .kind = EFFECT_TERM_UNKNOWN,
        .expr = expr,
    };
    effect_sig_push_term(sig, term);
}

struct EffectSig* sema_effect_sig_from_expr(struct Sema* s, struct Expr* effect) {
    if (!effect) return NULL;
    struct EffectSig* existing = effect_sig_find_existing(s, effect);
    if (existing) return existing;

    struct EffectSig* sig = effect_sig_new(s, effect);
    if (!sig) return NULL;
    effect_sig_collect_term(sig, effect);
    vec_push(s->effect_sigs, &sig);
    return sig;
}

static void print_effect_term(struct Sema* s, struct EffectTerm* term, bool first) {
    if (!term) return;
    if (term->kind == EFFECT_TERM_ROW) {
        const char* row_name = pool_get(s->pool, term->row_name_id, 0);
        printf(first ? "| %s" : " | %s", row_name ? row_name : "?");
        return;
    }

    if (!first) printf(", ");
    const char* name = pool_get(s->pool, term->name_id, 0);
    switch (term->kind) {
        case EFFECT_TERM_SCOPED:
            printf("%s(scope#%u)", name ? name : "?", term->scope_token_id);
            break;
        case EFFECT_TERM_NAMED:
            printf("%s", name ? name : "?");
            break;
        case EFFECT_TERM_UNKNOWN:
            printf("?");
            break;
        case EFFECT_TERM_ROW:
            break;
    }
}

void sema_print_effect_sig(struct Sema* s, struct EffectSig* sig) {
    printf("<");
    if (!sig || !sig->terms || sig->terms->count == 0) {
        printf("pure");
    } else {
        for (size_t i = 0; i < sig->terms->count; i++) {
            struct EffectTerm* term = (struct EffectTerm*)vec_get(sig->terms, i);
            print_effect_term(s, term, i == 0);
        }
    }
    printf(">");
}