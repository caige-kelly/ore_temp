#include "sema.h"

#include <stdarg.h>
#include <stdio.h>

#include "checker.h"
#include "comptime.h"
#include "decls.h"
#include "effects.h"
#include "sema_internal.h"
#include "type.h"
#include "../compiler/compiler.h"

void sema_error(struct Sema* s, struct Span span, const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (s->diags) {
        diag_error(s->diags, span, "%s", msg);
    }
    s->has_errors = true;
}

void sema_record_fact(struct Sema* s, struct Expr* expr, struct Type* type,
    SemanticKind semantic_kind, uint32_t region_id) {
    if (!expr) return;
    struct SemaFact fact = {
        .expr = expr,
        .type = type ? type : s->unknown_type,
        .semantic_kind = semantic_kind,
        .region_id = region_id,
    };
    vec_push(s->facts, &fact);
}

struct SemaFact* sema_fact_of(struct Sema* s, struct Expr* expr) {
    if (!s || !expr || !s->facts) return NULL;
    for (size_t i = s->facts->count; i > 0; i--) {
        struct SemaFact* fact = (struct SemaFact*)vec_get(s->facts, i - 1);
        if (fact && fact->expr == expr) return fact;
    }
    return NULL;
}

struct Type* sema_type_of(struct Sema* s, struct Expr* expr) {
    struct SemaFact* fact = sema_fact_of(s, expr);
    return fact ? fact->type : NULL;
}

SemanticKind sema_semantic_of(struct Sema* s, struct Expr* expr) {
    struct SemaFact* fact = sema_fact_of(s, expr);
    return fact ? fact->semantic_kind : SEM_UNKNOWN;
}

uint32_t sema_region_of(struct Sema* s, struct Expr* expr) {
    struct SemaFact* fact = sema_fact_of(s, expr);
    return fact ? fact->region_id : 0;
}

struct EffectSig* sema_effect_sig_of(struct Sema* s, struct Expr* expr) {
    struct Type* type = sema_type_of(s, expr);
    if (type && type->effect_sig) return type->effect_sig;
    if (!s || !expr || !s->effect_sigs) return NULL;
    for (size_t i = s->effect_sigs->count; i > 0; i--) {
        struct EffectSig** sig_p = (struct EffectSig**)vec_get(s->effect_sigs, i - 1);
        if (sig_p && *sig_p && (*sig_p)->source == expr) return *sig_p;
    }
    return NULL;
}

struct Sema sema_new(struct Compiler* compiler, struct Resolver* resolver) {
    struct Sema s = {0};
    if (!compiler) return s;

    s.compiler = compiler;
    s.arena = &compiler->arena;
    s.pool = &compiler->pool;
    s.resolver = resolver;
    s.diags = &compiler->diags;
    s.facts = vec_new_in(&compiler->arena, sizeof(struct SemaFact));
    s.effect_sigs = vec_new_in(&compiler->arena, sizeof(struct EffectSig*));
    s.has_errors = false;

    s.unknown_type = sema_type_new(&s, TYPE_UNKNOWN);
    s.error_type = sema_type_new(&s, TYPE_ERROR);
    s.void_type = sema_type_new(&s, TYPE_VOID);
    s.bool_type = sema_type_new(&s, TYPE_BOOL);
    s.int_type = sema_type_new(&s, TYPE_INT);
    s.float_type = sema_type_new(&s, TYPE_FLOAT);
    s.string_type = sema_type_new(&s, TYPE_STRING);
    s.nil_type = sema_type_new(&s, TYPE_NIL);
    s.type_type = sema_type_new(&s, TYPE_TYPE);
    s.anytype_type = sema_type_new(&s, TYPE_ANYTYPE);
    s.module_type = sema_type_new(&s, TYPE_MODULE);
    s.effect_type = sema_type_new(&s, TYPE_EFFECT);
    s.effect_row_type = sema_type_new(&s, TYPE_EFFECT_ROW);
    s.scope_token_type = sema_type_new(&s, TYPE_SCOPE_TOKEN);
    return s;
}

bool sema_check(struct Sema* s) {
    if (!s || !s->resolver) return false;

    if (!sema_collect_declarations(s)) return false;
    if (!sema_prepare_comptime(s)) return false;
    if (!sema_check_expressions(s)) return false;

    return !s->has_errors;
}

void dump_sema(struct Sema* s) {
    if (!s) return;
    printf("\n=== sema skeleton ===\n");
    printf("  facts:  %zu\n", s->facts ? s->facts->count : 0);
    printf("  effect sigs: %zu\n", s->effect_sigs ? s->effect_sigs->count : 0);
    printf("  errors: %zu\n", s->diags ? s->diags->error_count : 0);

    size_t counts[TYPE_PRODUCT + 1] = {0};
    if (s->facts) {
        for (size_t i = 0; i < s->facts->count; i++) {
            struct SemaFact* fact = (struct SemaFact*)vec_get(s->facts, i);
            if (!fact || !fact->type) continue;
            if (fact->type->kind <= TYPE_PRODUCT) counts[fact->type->kind]++;
        }
    }

    printf("  type facts:\n");
    for (int i = 0; i <= TYPE_PRODUCT; i++) {
        if (counts[i] == 0) continue;
        printf("    %-12s %zu\n", sema_type_kind_str((TypeKind)i), counts[i]);
    }

    if (s->effect_sigs && s->effect_sigs->count > 0) {
        printf("  effect signatures:\n");
        size_t shown_sigs = 0;
        for (size_t i = 0; i < s->effect_sigs->count && shown_sigs < 12; i++) {
            struct EffectSig** sig_p = (struct EffectSig**)vec_get(s->effect_sigs, i);
            struct EffectSig* sig = sig_p ? *sig_p : NULL;
            if (!sig) continue;
            printf("    line %d col %d: ",
                sig->source ? sig->source->span.line : 0,
                sig->source ? sig->source->span.column : 0);
            sema_print_effect_sig(s, sig);
            if (sig->row_decl && sig->row_decl->semantic_kind == SEM_EFFECT_ROW) {
                const char* row_name = pool_get(s->pool, sig->row_name_id, 0);
                printf("  open-row=%s", row_name ? row_name : "?");
            }
            printf("\n");
            shown_sigs++;
        }
    }

    size_t shown = 0;
    if (s->facts && s->facts->count > 0) {
        printf("  first facts (semantic -> type):\n");
        for (size_t i = 0; i < s->facts->count && shown < 12; i++) {
            struct SemaFact* fact = (struct SemaFact*)vec_get(s->facts, i);
            if (!fact || !fact->type) continue;
            printf("    line %d col %d: %s -> %s",
                fact->expr ? fact->expr->span.line : 0,
                fact->expr ? fact->expr->span.column : 0,
                sema_semantic_kind_str(fact->semantic_kind),
                sema_type_kind_str(fact->type->kind));
            if (fact->region_id) printf(" @region#%u", fact->region_id);
            printf("\n");
            shown++;
        }
    }

}

void dump_sema_effects(struct Sema* s) {
    if (!s) return;
    printf("\n=== effect signatures ===\n");
    printf("  count: %zu\n", s->effect_sigs ? s->effect_sigs->count : 0);
    if (!s->effect_sigs) return;

    for (size_t i = 0; i < s->effect_sigs->count; i++) {
        struct EffectSig** sig_p = (struct EffectSig**)vec_get(s->effect_sigs, i);
        struct EffectSig* sig = sig_p ? *sig_p : NULL;
        if (!sig) continue;
        printf("  line %d col %d: ",
            sig->source ? sig->source->span.line : 0,
            sig->source ? sig->source->span.column : 0);
        sema_print_effect_sig(s, sig);
        if (sig->row_decl && sig->row_decl->semantic_kind == SEM_EFFECT_ROW) {
            const char* row_name = pool_get(s->pool, sig->row_name_id, 0);
            printf("  open-row=%s", row_name ? row_name : "?");
        }
        printf("\n");
    }
}
