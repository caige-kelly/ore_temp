#include "sema.h"

#include <stdarg.h>
#include <stdio.h>

#include "checker.h"
#include "const_eval.h"
#include "decls.h"
#include "effects.h"
#include "evidence.h"
#include "sema_internal.h"
#include "const_eval.h"
#include "type.h"
#include "../compiler/compiler.h"

// ----- Per-Decl sema cache (see sema_internal.h::SemaDeclInfo) -----

struct SemaDeclInfo* sema_decl_info(struct Sema* s, struct Decl* decl) {
    if (!s || !decl) return NULL;
    struct SemaDeclInfo* info = (struct SemaDeclInfo*)hashmap_get(
        &s->decl_info, (uint64_t)(uintptr_t)decl);
    if (info) return info;
    info = arena_alloc(s->arena, sizeof(struct SemaDeclInfo));
    if (!info) return NULL;
    sema_query_slot_init(&info->type_query, QUERY_TYPE_OF_DECL);
    sema_query_slot_init(&info->effect_sig_query, QUERY_EFFECT_SIG);
    sema_query_slot_init(&info->body_effects_query, QUERY_BODY_EFFECTS);
    info->type = NULL;
    info->effect_sig = NULL;
    info->body_effects = NULL;
    hashmap_put(&s->decl_info, (uint64_t)(uintptr_t)decl, info);
    return info;
}

struct Type* sema_decl_type(struct Sema* s, struct Decl* decl) {
    struct SemaDeclInfo* info = sema_decl_info(s, decl);
    return info ? info->type : NULL;
}

struct EffectSig* sema_decl_effect_sig(struct Sema* s, struct Decl* decl) {
    struct SemaDeclInfo* info = sema_decl_info(s, decl);
    return info ? info->effect_sig : NULL;
}

struct EffectSet* sema_decl_body_effects(struct Sema* s, struct Decl* decl) {
    struct SemaDeclInfo* info = sema_decl_info(s, decl);
    return info ? info->body_effects : NULL;
}

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

struct CheckedBody* sema_body_new(struct Sema* s, struct Decl* decl,
    struct Module* module, struct Instantiation* instantiation) {
    if (!s || !s->arena) return NULL;
    struct CheckedBody* body = arena_alloc(s->arena, sizeof(struct CheckedBody));
    if (!body) return NULL;
    body->decl = decl;
    body->module = module;
    body->instantiation = instantiation;
    body->facts = vec_new_in(s->arena, sizeof(struct SemaFact));
    body->entry_evidence = sema_evidence_clone(s, s->current_evidence);
    hashmap_init_in(&body->call_evidence, s->arena);
    if (s->bodies) vec_push(s->bodies, &body);
    return body;
}

struct CheckedBody* sema_enter_body(struct Sema* s, struct CheckedBody* body) {
    if (!s) return NULL;
    struct CheckedBody* prev = s->current_body;
    s->current_body = body;
    return prev;
}

void sema_leave_body(struct Sema* s, struct CheckedBody* previous) {
    if (!s) return;
    s->current_body = previous;
}

void body_record_fact(struct Sema* s, struct CheckedBody* body, struct Expr* expr,
    struct Type* type, SemanticKind semantic_kind, uint32_t region_id) {
    if (!s || !body || !body->facts || !expr) return;
    struct SemaFact fact = {
        .expr = expr,
        .type = type ? type : s->unknown_type,
        .semantic_kind = semantic_kind,
        .region_id = region_id,
    };
    vec_push(body->facts, &fact);
}

void sema_record_fact(struct Sema* s, struct Expr* expr, struct Type* type,
    SemanticKind semantic_kind, uint32_t region_id) {
    if (!s || !expr) return;
    if (!s->current_body) {
        // Surface the bug instead of silently dropping. Once per process is
        // enough — the same site usually fires repeatedly otherwise.
        static bool warned = false;
        if (!warned) {
            fprintf(stderr,
                "warning: sema_record_fact called with no current_body "
                "(line %d); fact discarded\n", expr->span.line);
            warned = true;
        }
        return;
    }
    body_record_fact(s, s->current_body, expr, type, semantic_kind, region_id);
}

static struct SemaFact* find_fact_in_body(struct CheckedBody* body, struct Expr* expr) {
    if (!body || !body->facts) return NULL;
    for (size_t i = body->facts->count; i > 0; i--) {
        struct SemaFact* fact = (struct SemaFact*)vec_get(body->facts, i - 1);
        if (fact && fact->expr == expr) return fact;
    }
    return NULL;
}

// TODO(perf): on miss, this walks every CheckedBody linearly. Fine while the
// only consumers are dump/diagnostic readers. When codegen starts looking up
// facts at scale, add a Sema-side `Expr* -> CheckedBody*` reverse index that
// each body update keeps current.
struct SemaFact* sema_fact_of(struct Sema* s, struct Expr* expr) {
    if (!s || !expr) return NULL;
    struct SemaFact* hit = find_fact_in_body(s->current_body, expr);
    if (hit) return hit;
    if (s->bodies) {
        for (size_t i = s->bodies->count; i > 0; i--) {
            struct CheckedBody** body_p = (struct CheckedBody**)vec_get(s->bodies, i - 1);
            struct CheckedBody* body = body_p ? *body_p : NULL;
            if (body == s->current_body) continue;
            struct SemaFact* found = find_fact_in_body(body, expr);
            if (found) return found;
        }
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
    if (!s || !expr) return NULL;
    return (struct EffectSig*)hashmap_get(&s->effect_sig_cache,
        (uint64_t)(uintptr_t)expr);
}

struct Sema sema_new(struct Compiler* compiler, struct Resolver* resolver) {
    struct Sema s = {0};
    if (!compiler) return s;

    s.compiler = compiler;
    s.arena = &compiler->arena;
    s.pool = &compiler->pool;
    s.resolver = resolver;
    s.diags = &compiler->diags;
    s.bodies = vec_new_in(&compiler->arena, sizeof(struct CheckedBody*));
    s.current_body = NULL;
    s.instantiations = vec_new_in(&compiler->arena, sizeof(struct Instantiation*));
    hashmap_init_in(&s.instantiation_buckets, &compiler->arena);
    hashmap_init_in(&s.decl_info, &compiler->arena);
    s.current_env = NULL;
    s.current_evidence = sema_evidence_new(&s);
    hashmap_init_in(&s.effect_sig_cache, &compiler->arena);
    s.query_stack = vec_new_in(&compiler->arena, sizeof(struct QueryFrame));
    s.comptime_call_depth = 0;
    hashmap_init_in(&s.call_cache, &compiler->arena);
    s.comptime_body_evals = 0;
    s.has_errors = false;

    s.unknown_type = sema_type_new(&s, TYPE_UNKNOWN);
    s.error_type = sema_type_new(&s, TYPE_ERROR);
    s.void_type = sema_type_new(&s, TYPE_VOID);
    s.noreturn_type = sema_type_new(&s, TYPE_NORETURN);
    s.bool_type = sema_type_new(&s, TYPE_BOOL);
    s.comptime_int_type = sema_type_new(&s, TYPE_COMPTIME_INT);
    s.comptime_float_type = sema_type_new(&s, TYPE_COMPTIME_FLOAT);
    s.u8_type = sema_type_new(&s, TYPE_U8);
    s.const_u8_type = sema_const_qualified_type(&s, s.u8_type);
    s.u16_type = sema_type_new(&s, TYPE_U16);
    s.u32_type = sema_type_new(&s, TYPE_U32);
    s.u64_type = sema_type_new(&s, TYPE_U64);
    s.usize_type = sema_type_new(&s, TYPE_USIZE);
    s.i8_type = sema_type_new(&s, TYPE_I8);
    s.i16_type = sema_type_new(&s, TYPE_I16);
    s.i32_type = sema_type_new(&s, TYPE_I32);
    s.i64_type = sema_type_new(&s, TYPE_I64);
    s.isize_type = sema_type_new(&s, TYPE_ISIZE);
    s.f32_type = sema_type_new(&s, TYPE_F32);
    s.f64_type = sema_type_new(&s, TYPE_F64);
    s.string_type = sema_type_new(&s, TYPE_STRING);
    s.nil_type = sema_type_new(&s, TYPE_NIL);
    s.type_type = sema_type_new(&s, TYPE_TYPE);
    s.anytype_type = sema_type_new(&s, TYPE_ANYTYPE);
    s.module_type = sema_type_new(&s, TYPE_MODULE);
    s.effect_type = sema_type_new(&s, TYPE_EFFECT);
    s.effect_row_type = sema_type_new(&s, TYPE_EFFECT_ROW);
    s.scope_token_type = sema_type_new(&s, TYPE_SCOPE_TOKEN);

    // Pre-intern hot-path name IDs (see sema.h for rationale).
    s.name_import  = pool_intern(s.pool, "import",  6);
    s.name_sizeOf  = pool_intern(s.pool, "sizeOf",  6);
    s.name_alignOf = pool_intern(s.pool, "alignOf", 7);
    s.name_intCast = pool_intern(s.pool, "intCast", 7);
    s.name_TypeOf  = pool_intern(s.pool, "TypeOf",  6);
    s.name_target     = pool_intern(s.pool, "target",     6);
    s.name_true       = pool_intern(s.pool, "true",       4);
    s.name_false      = pool_intern(s.pool, "false",      5);
    s.name_returnType = pool_intern(s.pool, "returnType", 10);

    // Build the primitive-name → Type* table once. Mirrors the
    // resolver's `register_primitives` list plus the comptime numerics
    // that aren't user-facing identifiers.
    hashmap_init_in(&s.primitive_types, &compiler->arena);
    #define ORE_REG_PRIM(NAME, TYPE_PTR) \
        hashmap_put(&s.primitive_types, \
            (uint64_t)pool_intern(s.pool, NAME, sizeof(NAME) - 1), \
            (TYPE_PTR))
    ORE_REG_PRIM("void",           s.void_type);
    ORE_REG_PRIM("noreturn",       s.noreturn_type);
    ORE_REG_PRIM("bool",           s.bool_type);
    ORE_REG_PRIM("type",           s.type_type);
    ORE_REG_PRIM("anytype",        s.anytype_type);
    ORE_REG_PRIM("Scope",          s.type_type);
    ORE_REG_PRIM("nil",            s.nil_type);
    ORE_REG_PRIM("u8",             s.u8_type);
    ORE_REG_PRIM("u16",            s.u16_type);
    ORE_REG_PRIM("u32",            s.u32_type);
    ORE_REG_PRIM("u64",            s.u64_type);
    ORE_REG_PRIM("usize",          s.usize_type);
    ORE_REG_PRIM("i8",             s.i8_type);
    ORE_REG_PRIM("i16",            s.i16_type);
    ORE_REG_PRIM("i32",            s.i32_type);
    ORE_REG_PRIM("i64",            s.i64_type);
    ORE_REG_PRIM("isize",          s.isize_type);
    ORE_REG_PRIM("f32",            s.f32_type);
    ORE_REG_PRIM("f64",            s.f64_type);
    ORE_REG_PRIM("comptime_int",   s.comptime_int_type);
    ORE_REG_PRIM("comptime_float", s.comptime_float_type);
    // `true` / `false` are values typed as bool; the resolver classifies
    // them, but `sema_primitive_type_for_name` historically returned bool
    // for either name so we preserve that.
    hashmap_put(&s.primitive_types, (uint64_t)s.name_true,  s.bool_type);
    hashmap_put(&s.primitive_types, (uint64_t)s.name_false, s.bool_type);
    #undef ORE_REG_PRIM
    return s;
}

void sema_record_call_value(struct Sema* s, struct Expr* call_expr, struct ConstValue v) {
    struct SemaFact* fact = sema_fact_of(s, call_expr);
    if (!fact) return;
    fact->value = v;
}

bool sema_check(struct Sema* s) {
    if (!s || !s->resolver) return false;

    // Signature resolution can produce facts (typechecking inside type
    // annotations, default values, etc.) before per-module bodies exist.
    // We give it a dedicated scratch body so:
    //   - sema_record_fact's no-current-body warning stays meaningful (it
    //     only fires for genuine bugs, not for signature work);
    //   - facts produced during sig resolution survive in case future
    //     analyses (cross-decl type queries) want them;
    //   - the body shows up in --dump-evidence as <sig-resolution-scratch>
    //     so it's visibly distinct from real per-decl/per-module bodies.
    // It has decl=NULL, module=NULL, instantiation=NULL — that triple is
    // the tell.
    struct CheckedBody* sig_body = sema_body_new(s, NULL, NULL, NULL);
    struct CheckedBody* prev = sema_enter_body(s, sig_body);

    bool ok_decls = sema_collect_declarations(s);

    sema_leave_body(s, prev);

    if (!ok_decls) return false;
    if (!sema_check_expressions(s)) return false;

    return !s->has_errors;
}

static size_t total_fact_count(struct Sema* s) {
    if (!s || !s->bodies) return 0;
    size_t total = 0;
    for (size_t i = 0; i < s->bodies->count; i++) {
        struct CheckedBody** body_p = (struct CheckedBody**)vec_get(s->bodies, i);
        struct CheckedBody* body = body_p ? *body_p : NULL;
        if (body && body->facts) total += body->facts->count;
    }
    return total;
}

// ----- hashmap-foreach visitors for the dump functions -----

struct EffectSigDumpCtx {
    struct Sema* s;
    size_t shown;
    size_t limit;       // 0 = unlimited
    const char* prefix; // line prefix (indentation)
};

static bool dump_effect_sig_visitor(uint64_t key, void* value, void* user) {
    (void)key;
    struct EffectSigDumpCtx* c = user;
    if (c->limit && c->shown >= c->limit) return false;
    struct EffectSig* sig = (struct EffectSig*)value;
    if (!sig) return true;
    printf("%sline %d col %d: ", c->prefix ? c->prefix : "",
        sig->source ? sig->source->span.line : 0,
        sig->source ? sig->source->span.column : 0);
    sema_print_effect_sig(c->s, sig);
    if (sig->row_decl && sig->row_decl->semantic_kind == SEM_EFFECT_ROW) {
        const char* row_name = pool_get(c->s->pool, sig->row_name_id, 0);
        printf("  open-row=%s", row_name ? row_name : "?");
    }
    printf("\n");
    c->shown++;
    return true;
}

struct CallEvidenceDumpCtx {
    struct Sema* s;
    const char* prefix;
};

static void print_evidence_vector(struct Sema* s, struct EvidenceVector* ev,
    const char* prefix);

static bool dump_call_evidence_visitor(uint64_t key, void* value, void* user) {
    struct CallEvidenceDumpCtx* c = user;
    struct Expr* call = (struct Expr*)(uintptr_t)key;
    struct EvidenceVector* ev = (struct EvidenceVector*)value;
    printf("      call @ line %d col %d:\n",
        call ? call->span.line : 0,
        call ? call->span.column : 0);
    print_evidence_vector(c->s, ev, "        ");
    return true;
}

static void tally_facts_by_kind(struct Sema* s, size_t counts[TYPE_PRODUCT + 1]) {
    if (!s || !s->bodies) return;
    for (size_t i = 0; i < s->bodies->count; i++) {
        struct CheckedBody** body_p = (struct CheckedBody**)vec_get(s->bodies, i);
        struct CheckedBody* body = body_p ? *body_p : NULL;
        if (!body || !body->facts) continue;
        for (size_t j = 0; j < body->facts->count; j++) {
            struct SemaFact* fact = (struct SemaFact*)vec_get(body->facts, j);
            if (!fact || !fact->type) continue;
            if (fact->type->kind <= TYPE_PRODUCT) counts[fact->type->kind]++;
        }
    }
}

void dump_tyck(struct Sema* s) {
    if (!s) return;
    printf("\n=== sema typechecking ===\n");
    printf("  facts:  %zu\n", total_fact_count(s));

    size_t counts[TYPE_PRODUCT + 1] = {0};
    tally_facts_by_kind(s, counts);

    printf("  type facts:\n");
    for (int i = 0; i <= TYPE_PRODUCT; i++) {
        if (counts[i] == 0) continue;
        printf("    %-12s %zu\n", sema_type_kind_str((TypeKind)i), counts[i]);
    }

    if (s->compiler && s->compiler->modules) {
        bool printed_header = false;
        for (size_t i = 0; i < s->compiler->modules->count; i++) {
            struct Module** mod_p = (struct Module**)vec_get(s->compiler->modules, i);
            struct Module* mod = mod_p ? *mod_p : NULL;
            if (!mod || !mod->ast) continue;

            for (size_t j = 0; j < mod->ast->count; j++) {
                struct Expr** expr_p = (struct Expr**)vec_get(mod->ast, j);
                struct Expr* expr = expr_p ? *expr_p : NULL;
                if (!expr || expr->kind != expr_Bind) continue;
                if (!expr->bind.name.resolved) continue;

                struct ConstValue v = sema_decl_value(s, expr->bind.name.resolved);
                if (v.kind == CONST_INVALID) continue;

                if (!printed_header) {
                    printf("  comptime values:\n");
                    printed_header = true;
                }

                const char* name = s->pool
                    ? pool_get(s->pool, expr->bind.name.string_id, 0) : NULL;
                printf("    %-12s = ", name ? name : "?");
                sema_print_const_value(v, s);
                printf("\n");
            }
        }
    }

    // Print folded call values
    bool printed_calls = false;
    if (s->bodies) {
        for (size_t b = 0; b < s->bodies->count; b++) {
            struct CheckedBody** body_p = (struct CheckedBody**)vec_get(s->bodies, b);
            struct CheckedBody* body = body_p ? *body_p : NULL;
            if (!body || !body->facts) continue;

            for (size_t i = 0; i < body->facts->count; i++) {
                struct SemaFact* f = (struct SemaFact*)vec_get(body->facts, i);
                if (!f || !f->expr || f->expr->kind != expr_Call) continue;
                if (f->value.kind == CONST_INVALID) continue;

                if (!printed_calls) {
                    printf("  folded calls:\n");
                    printed_calls = true;
                }
                printf("    line %d: ", f->expr->span.line);
                sema_print_const_value(f->value, s);
                printf("\n");
            }
        }
    }
}

void dump_sema(struct Sema* s) {
    if (!s) return;
    printf("\n=== sema skeleton ===\n");
    printf("  facts:  %zu\n", total_fact_count(s));
    printf("  effect sigs: %zu\n", s->effect_sig_cache.count);
    printf("  errors: %zu\n", s->diags ? s->diags->error_count : 0);

    size_t counts[TYPE_PRODUCT + 1] = {0};
    tally_facts_by_kind(s, counts);

    printf("  type facts:\n");
    for (int i = 0; i <= TYPE_PRODUCT; i++) {
        if (counts[i] == 0) continue;
        printf("    %-12s %zu\n", sema_type_kind_str((TypeKind)i), counts[i]);
    }

    if (s->effect_sig_cache.count > 0) {
        printf("  effect signatures:\n");
        struct EffectSigDumpCtx ctx = {
            .s = s, .shown = 0, .limit = 12, .prefix = "    "
        };
        hashmap_foreach(&s->effect_sig_cache, dump_effect_sig_visitor, &ctx);
    }

    size_t shown = 0;
    if (s->bodies && total_fact_count(s) > 0) {
        printf("  first facts (semantic -> type):\n");
        for (size_t b = 0; b < s->bodies->count && shown < 12; b++) {
            struct CheckedBody** body_p = (struct CheckedBody**)vec_get(s->bodies, b);
            struct CheckedBody* body = body_p ? *body_p : NULL;
            if (!body || !body->facts) continue;
            for (size_t i = 0; i < body->facts->count && shown < 12; i++) {
                struct SemaFact* fact = (struct SemaFact*)vec_get(body->facts, i);
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
}

static void print_evidence_vector(struct Sema* s, struct EvidenceVector* ev,
    const char* prefix) {
    if (!ev || !ev->frames || ev->frames->count == 0) {
        printf("%s<empty>\n", prefix);
        return;
    }
    for (size_t i = 0; i < ev->frames->count; i++) {
        struct EvidenceFrame* f = (struct EvidenceFrame*)vec_get(ev->frames, i);
        if (!f) continue;
        const char* eff_name = (f->effect_decl && s->pool)
            ? pool_get(s->pool, f->effect_decl->name.string_id, 0) : NULL;
        const char* h_name = (f->handler_decl && s->pool)
            ? pool_get(s->pool, f->handler_decl->name.string_id, 0) : NULL;
        const char* depth_label = (i == 0) ? "outermost"
            : (i + 1 == ev->frames->count) ? "innermost" : "         ";
        printf("%s[%zu %s] effect=%s handler=%s",
            prefix, i, depth_label,
            eff_name ? eff_name : "?",
            h_name ? h_name : "?");
        if (f->scope_token_id) printf(" scope#%u", f->scope_token_id);
        printf("\n");
    }
}

void dump_sema_evidence(struct Sema* s) {
    if (!s) return;
    printf("\n=== evidence vectors ===\n");
    if (!s->bodies) { printf("  no bodies\n"); return; }

    for (size_t i = 0; i < s->bodies->count; i++) {
        struct CheckedBody** bp = (struct CheckedBody**)vec_get(s->bodies, i);
        struct CheckedBody* body = bp ? *bp : NULL;
        if (!body) continue;
        const char* nm;
        if (body->decl && s->pool) {
            nm = pool_get(s->pool, body->decl->name.string_id, 0);
        } else if (body->module) {
            nm = "<module>";
        } else {
            nm = "<sig-resolution-scratch>";
        }
        printf("  body '%s'%s:\n",
            nm ? nm : "<unnamed>",
            body->instantiation ? " (instantiation)" : "");
        printf("    entry-evidence:\n");
        print_evidence_vector(s, body->entry_evidence, "      ");

        if (body->call_evidence.count == 0) continue;
        printf("    per-call snapshots: %zu\n", body->call_evidence.count);
        struct CallEvidenceDumpCtx ctx = { .s = s };
        hashmap_foreach(&body->call_evidence, dump_call_evidence_visitor, &ctx);
    }
}

void dump_sema_effects(struct Sema* s) {
    if (!s) return;
    printf("\n=== effect signatures ===\n");
    printf("  count: %zu\n", s->effect_sig_cache.count);

    struct EffectSigDumpCtx ctx = {
        .s = s, .shown = 0, .limit = 0, .prefix = "  "
    };
    hashmap_foreach(&s->effect_sig_cache, dump_effect_sig_visitor, &ctx);
}
