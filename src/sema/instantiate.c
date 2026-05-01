#include "instantiate.h"

#include "checker.h"
#include "effect_solver.h"
#include "effects.h"
#include "sema.h"
#include "sema/const_eval.h"
#include "sema_internal.h"
#include "type.h"
#include "../compiler/compiler.h"
#include "../parser/ast.h"

bool sema_param_is_comptime(struct Param* param) {
    return param && param->kind != PARAM_RUNTIME;
}

bool sema_param_is_inferred(struct Param* param) {
    return param && param->kind == PARAM_INFERRED_COMPTIME;
}

size_t sema_param_visible_arity(Vec* params) {
    if (!params) return 0;
    size_t n = 0;
    for (size_t i = 0; i < params->count; i++) {
        struct Param* p = (struct Param*)vec_get(params, i);
        if (!p) continue;
        // Inferred params are filled by sema from context, never written by
        // the caller — they don't count toward visible arity.
        if (p->kind == PARAM_INFERRED_COMPTIME) continue;
        n++;
    }
    return n;
}

Vec* sema_decl_function_params(struct Decl* decl) {
    if (!decl || !decl->node) return NULL;
    // Param/field/scope-token decls share their owner's AST node; they
    // aren't function-defining decls themselves.
    if (decl->kind != DECL_USER && decl->kind != DECL_PRIMITIVE &&
        decl->kind != DECL_IMPORT) return NULL;
    struct Expr* node = decl->node;
    if (node->kind == expr_Lambda) return node->lambda.params;
    if (node->kind == expr_Ctl) return node->ctl.params;
    if (node->kind == expr_Bind && node->bind.value) {
        struct Expr* v = node->bind.value;
        if (v->kind == expr_Lambda) return v->lambda.params;
        if (v->kind == expr_Ctl) return v->ctl.params;
    }
    return NULL;
}

struct Expr* sema_decl_function_body(struct Decl* decl) {
    if (!decl || !decl->node) return NULL;
    struct Expr* node = decl->node;
    if (node->kind == expr_Lambda) return node->lambda.body;
    if (node->kind == expr_Ctl) return node->ctl.body;
    if (node->kind == expr_Bind && node->bind.value) {
        struct Expr* v = node->bind.value;
        if (v->kind == expr_Lambda) return v->lambda.body;
        if (v->kind == expr_Ctl) return v->ctl.body;
    }
    return NULL;
}

struct Expr* sema_decl_function_ret_type_expr(struct Decl* decl) {
    if (!decl || !decl->node) return NULL;
    struct Expr* node = decl->node;
    if (node->kind == expr_Lambda) return node->lambda.ret_type;
    if (node->kind == expr_Ctl) return node->ctl.ret_type;
    if (node->kind == expr_Bind && node->bind.value) {
        struct Expr* v = node->bind.value;
        if (v->kind == expr_Lambda) return v->lambda.ret_type;
        if (v->kind == expr_Ctl) return v->ctl.ret_type;
    }
    return NULL;
}

size_t sema_decl_comptime_param_count(struct Decl* decl) {
    Vec* params = sema_decl_function_params(decl);
    if (!params) return 0;
    size_t count = 0;
    for (size_t i = 0; i < params->count; i++) {
        struct Param* p = (struct Param*)vec_get(params, i);
        if (sema_param_is_comptime(p)) count++;
    }
    return count;
}

bool sema_decl_is_generic(struct Decl* decl) {
    return sema_decl_comptime_param_count(decl) > 0;
}

// ----- ComptimeArgTuple -----

struct ComptimeArgTuple sema_arg_tuple_new(struct Sema* s) {
    struct ComptimeArgTuple t = {0};
    if (s && s->arena) t.values = vec_new_in(s->arena, sizeof(struct ConstValue));
    return t;
}

void sema_arg_tuple_push(struct ComptimeArgTuple* tuple, struct ConstValue value) {
    if (!tuple || !tuple->values) return;
    vec_push(tuple->values, &value);
}

bool sema_arg_tuple_equal(const struct ComptimeArgTuple* a, const struct ComptimeArgTuple* b) {
    if (!a || !b) return a == b;
    Vec* av = a->values;
    Vec* bv = b->values;
    if (!av || !bv) return av == bv;
    if (av->count != bv->count) return false;
    for (size_t i = 0; i < av->count; i++) {
        struct ConstValue* ax = (struct ConstValue*)vec_get(av, i);
        struct ConstValue* bx = (struct ConstValue*)vec_get(bv, i);
        if (!ax || !bx) return false;
        if (ax->kind != bx->kind) return false;
        switch (ax->kind) {
            case CONST_INT:      if (ax->int_val != bx->int_val) return false; break;
            case CONST_FLOAT:    if (ax->float_val != bx->float_val) return false; break;
            case CONST_BOOL:     if (ax->bool_val != bx->bool_val) return false; break;
            case CONST_TYPE:     if (!sema_type_equal(ax->type_val, bx->type_val)) return false; break;
            case CONST_STRING:   if (ax->string_id != bx->string_id) return false; break;
            case CONST_FUNCTION: if (ax->fn_decl != bx->fn_decl) return false; break;
            case CONST_STRUCT: {
                struct ConstStruct* a_sv = ax->struct_val;
                struct ConstStruct* b_sv = bx->struct_val;
                if (!a_sv || !b_sv) { if (a_sv != b_sv) return false; break; }
                if (a_sv->type != b_sv->type) return false;
                if (!a_sv->fields || !b_sv->fields) { if (a_sv->fields != b_sv->fields) return false; break; }
                if (a_sv->fields->count != b_sv->fields->count) return false;
                // Could call into sema_const_value_equal here recursively, but the
                // existing pattern in this switch is inline equality — match the style.
                for (size_t i = 0; i < a_sv->fields->count; i++) {
                    struct ConstStructField* af = (struct ConstStructField*)vec_get(a_sv->fields, i);
                    struct ConstStructField* bf = (struct ConstStructField*)vec_get(b_sv->fields, i);
                    if (!af || !bf) return false;
                    if (af->name_id != bf->name_id) return false;
                    if (!sema_const_value_equal(af->value, bf->value)) return false;
                }
                break;
            }
            case CONST_ARRAY:   if (ax->array_val->elements != bx->array_val->elements) return false; break;
            case CONST_VOID:     break;
            case CONST_INVALID:  return false;
        }
    }
    return true;
}

// ----- Instantiation cache + creation -----

static Vec* instantiation_bucket(struct Sema* s, struct Decl* generic, bool create) {
    Vec* bucket = (Vec*)hashmap_get(&s->instantiation_buckets, (uint64_t)(uintptr_t)generic);
    if (bucket || !create) return bucket;
    bucket = vec_new_in(s->arena, sizeof(struct Instantiation*));
    hashmap_put(&s->instantiation_buckets, (uint64_t)(uintptr_t)generic, bucket);
    return bucket;
}

static struct Instantiation* find_existing(struct Sema* s, struct Decl* generic,
    const struct ComptimeArgTuple* args) {
    if (!s) return NULL;
    Vec* bucket = instantiation_bucket(s, generic, false);
    if (!bucket) return NULL;
    for (size_t i = 0; i < bucket->count; i++) {
        struct Instantiation** ip = (struct Instantiation**)vec_get(bucket, i);
        struct Instantiation* inst = ip ? *ip : NULL;
        if (!inst) continue;
        if (sema_arg_tuple_equal(&inst->args, args)) return inst;
    }
    return NULL;
}

static struct Decl* find_param_decl_in(struct Scope* scope, uint32_t name_id) {
    if (!scope) return NULL;
    struct Decl* d = (struct Decl*)hashmap_get(&scope->name_index, (uint64_t)name_id);
    return (d && d->kind == DECL_PARAM) ? d : NULL;
}

static void bind_comptime_args(struct Sema* s, struct ComptimeEnv* env,
    struct Decl* generic, const struct ComptimeArgTuple* args) {
    Vec* params = sema_decl_function_params(generic);
    if (!params || !args || !args->values) return;

    size_t arg_idx = 0;
    for (size_t i = 0; i < params->count && arg_idx < args->values->count; i++) {
        struct Param* p = (struct Param*)vec_get(params, i);
        if (!p || p->kind == PARAM_RUNTIME) continue;
        struct ConstValue* v = (struct ConstValue*)vec_get(args->values, arg_idx);
        if (!v) { arg_idx++; continue; }

        struct Decl* pd = find_param_decl_in(generic->child_scope, p->name.string_id);
        if (pd) sema_comptime_env_bind(s, env, pd, *v);
        arg_idx++;
    }
}

// Build the specialized function type by re-evaluating each (non-comptime) param's
// type annotation and the return type with the instantiation env active.
static struct Type* build_specialized_type(struct Sema* s, struct Decl* generic) {
    struct Type* fn = sema_function_type(s);
    if (!fn) return s->error_type;
    fn->decl = generic;

    Vec* params = sema_decl_function_params(generic);
    if (params) {
        for (size_t i = 0; i < params->count; i++) {
            struct Param* p = (struct Param*)vec_get(params, i);
            if (!p) continue;
            if (p->kind != PARAM_RUNTIME) continue;  // erased — not part of the runtime signature
            struct Type* pt = p->type_ann
                ? sema_infer_type_expr(s, p->type_ann)
                : s->unknown_type;
            vec_push(fn->params, &pt);
        }
    }

    struct Expr* ret_expr = sema_decl_function_ret_type_expr(generic);
    fn->ret = ret_expr ? sema_infer_type_expr(s, ret_expr) : s->unknown_type;

    // Carry over the generic's effect signature for now. Phase 6 will substitute
    // effect rows that depend on comptime args.
    struct EffectSig* generic_sig = sema_decl_effect_sig(s, generic);
    if (generic_sig) {
        fn->effect_sig = generic_sig;
        if (fn->effects && fn->effects->count == 0) {
            vec_push(fn->effects, &generic_sig);
        }
    }

    return fn;
}

struct Instantiation* sema_instantiate_decl(struct Sema* s, struct Decl* generic,
    struct ComptimeArgTuple args) {
    if (!s || !generic) return NULL;

    struct Instantiation* hit = find_existing(s, generic, &args);
    if (hit) return hit;

    struct Instantiation* inst = arena_alloc(s->arena, sizeof(struct Instantiation));
    if (!inst) return NULL;
    inst->generic = generic;
    inst->args = args;
    inst->specialized_type = NULL;
    inst->specialized_sig = NULL;
    inst->body = NULL;
    inst->env = NULL;
    sema_query_slot_init(&inst->query, QUERY_INSTANTIATE_DECL);

    // Eagerly cache so recursive calls into the same instantiation hit.
    vec_push(s->instantiations, &inst);
    Vec* bucket = instantiation_bucket(s, generic, true);
    if (bucket) vec_push(bucket, &inst);

    QueryBeginResult begin = sema_query_begin(s, &inst->query, QUERY_INSTANTIATE_DECL,
        inst, generic->name.span);
    if (begin == QUERY_BEGIN_CYCLE) {
        sema_error(s, generic->name.span,
            "recursive instantiation of '%s' is not yet supported",
            s->pool ? pool_get(s->pool, generic->name.string_id, 0) : "?");
        return inst;
    }
    if (begin == QUERY_BEGIN_ERROR) return inst;
    if (begin == QUERY_BEGIN_CACHED) return inst;

    // Build env, push it as current.
    inst->env = sema_comptime_env_new(s, NULL);
    bind_comptime_args(s, inst->env, generic, &inst->args);

    struct ComptimeEnv* prev_env = s->current_env;
    s->current_env = inst->env;

    // Specialized signature.
    inst->specialized_type = build_specialized_type(s, generic);
    inst->specialized_sig = inst->specialized_type ? inst->specialized_type->effect_sig : NULL;

    // Per-instantiation body so any further checking lands in its own facts vec.
    struct Module* mod = s->current_body ? s->current_body->module : NULL;
    inst->body = sema_body_new(s, generic, mod, inst);

    // Re-walk the generic body with the instantiation env active. Interior
    // expression facts are recorded against this instantiation's CheckedBody
    // (not the generic's), so call sites with different comptime args produce
    // distinct fact tables for body-internal expressions like `dst^ = src^`.
    struct Expr* body_expr = sema_decl_function_body(generic);
    if (body_expr && inst->specialized_type) {
        struct CheckedBody* prev_body = sema_enter_body(s, inst->body);
        struct Type* ret = inst->specialized_type->ret;
        if (ret && !sema_type_is_errorish(ret) && ret->kind != TYPE_VOID) {
            sema_check_expr(s, body_expr, ret);
        } else {
            sema_infer_expr(s, body_expr);
        }
        sema_leave_body(s, prev_body);

        // Per-instantiation effect verification: each specialization may
        // perform a different effect set than the generic (different comptime
        // args produce different bodies). Run the solver against the
        // (potentially substituted) declared signature.
        struct EffectSet* inferred = sema_collect_effects_from_expr(s, body_expr);
        struct EffectSig* declared = inst->specialized_sig
            ? inst->specialized_sig
            : sema_decl_effect_sig(s, generic);
        sema_solve_effect_rows(s, generic, declared, inferred);
    }

    s->current_env = prev_env;

    sema_query_succeed(s, &inst->query);
    return inst;
}
