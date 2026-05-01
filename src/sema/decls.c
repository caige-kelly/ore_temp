#include "decls.h"

#include "checker.h"
#include "const_eval.h"
#include "effect_solver.h"
#include "effects.h"
#include "sema.h"
#include "sema_internal.h"
#include "type.h"
#include "decls.h"
#include "../compiler/compiler.h" 

static struct Type* compute_decl_signature(struct Sema* s, struct Decl* decl);

static void try_fold_decl_value(struct Sema* s, struct Decl* decl,
    struct SemaDeclInfo* info) {
    if (!decl || !info) return;
    if (decl->semantic_kind != SEM_VALUE) return;
    if (!decl->node || decl->node->kind != expr_Bind) return;
    if (!decl->node->bind.value) return;
    if (info->value.kind != CONST_INVALID) return;   // already folded
    if (info->fold_in_progress) return; 

    info->fold_in_progress = true;  
    struct EvalResult er = sema_const_eval_expr(s, decl->node->bind.value, NULL);
    info->fold_in_progress = false;
    
    if (er.control == EVAL_NORMAL && er.value.kind != CONST_INVALID) {
        info->value = er.value;
    }
}

static struct Expr* decl_bind_value(struct Decl* decl) {
    if (!decl || !decl->node) return NULL;
    if (decl->node->kind == expr_Bind) return decl->node->bind.value;
    return NULL;
}

static bool expr_is_function_like(struct Expr* expr) {
    return expr && (expr->kind == expr_Lambda || expr->kind == expr_Ctl);
}

static bool decl_is_function_like(struct Decl* decl) {
    if (!decl || !decl->node) return false;
    if (expr_is_function_like(decl->node)) return true;
    return expr_is_function_like(decl_bind_value(decl));
}

static struct Decl* scope_lookup_member(struct Scope* scope, uint32_t name_id) {
    if (!scope) return NULL;
    return (struct Decl*)hashmap_get(&scope->name_index, (uint64_t)name_id);
}

// TODO(perf): linear param-list walk. Called once per param resolution per
// function — fine for any plausible signature. Hashmap not justified.
static struct Param* find_param_decl(struct Decl* decl) {
    if (!decl || !decl->node) return NULL;

    Vec* params = NULL;
    if (decl->node->kind == expr_Lambda) {
        params = decl->node->lambda.params;
    } else if (decl->node->kind == expr_Ctl) {
        params = decl->node->ctl.params;
    }
    if (!params) return NULL;

    for (size_t i = 0; i < params->count; i++) {
        struct Param* param = (struct Param*)vec_get(params, i);
        if (param && param->name.string_id == decl->name.string_id) return param;
    }
    return NULL;
}

static void assign_decl_type(struct Sema* s, struct Decl* decl, struct Type* type) {
    if (!s || !decl || !type) return;
    struct SemaDeclInfo* info = sema_decl_info(s, decl);
    if (!info) return;
    info->type = type;
    info->type_query.state = type->kind == TYPE_ERROR ? QUERY_ERROR : QUERY_DONE;
    if (type->effect_sig) info->effect_sig = type->effect_sig;
}

static void allocate_skeleton(struct Sema* s, struct Decl* decl) {
    if (!s || !decl) return;
    struct SemaDeclInfo* info = sema_decl_info(s, decl);
    if (!info || info->type) return;

    switch (decl->semantic_kind) {
        case SEM_MODULE:
            info->type = sema_named_type(s, TYPE_MODULE, decl->name.string_id, decl);
            break;
        case SEM_EFFECT:
            info->type = sema_named_type(s, TYPE_EFFECT, decl->name.string_id, decl);
            break;
        case SEM_SCOPE_TOKEN:
            info->type = sema_named_type(s, TYPE_SCOPE_TOKEN, decl->name.string_id, decl);
            if (info->type) info->type->region_id = decl->scope_token_id;
            break;
        case SEM_EFFECT_ROW:
            info->type = sema_named_type(s, TYPE_EFFECT_ROW, decl->name.string_id, decl);
            break;
        case SEM_TYPE: {
            if (decl->kind == DECL_PRIMITIVE) {
                info->type = sema_primitive_type_for_name(s, decl->name.string_id);
                break;
            }

            struct Expr* value = decl_bind_value(decl);
            if (value && value->kind == expr_Struct) {
                info->type = sema_named_type(s, TYPE_STRUCT, decl->name.string_id, decl);
            } else if (value && value->kind == expr_Enum) {
                info->type = sema_named_type(s, TYPE_ENUM, decl->name.string_id, decl);
            } else {
                info->type = sema_named_type(s, TYPE_TYPE, decl->name.string_id, decl);
            }
            break;
        }
        case SEM_VALUE:
            if (decl->kind == DECL_PRIMITIVE) {
                info->type = sema_primitive_type_for_name(s, decl->name.string_id);
            } else if (decl->kind == DECL_IMPORT) {
                info->type = sema_named_type(s, TYPE_MODULE, decl->name.string_id, decl);
            } else if (decl_is_function_like(decl)) {
                info->type = sema_function_type(s);
                if (info->type) info->type->decl = decl;
            }
            break;
        case SEM_UNKNOWN:
        default:
            break;
    }
}

static void walk_scope_decls(struct Sema* s, struct Scope* scope,
    void (*visit)(struct Sema*, struct Decl*)) {
    if (!scope || !visit) return;

    if (scope->decls) {
        for (size_t i = 0; i < scope->decls->count; i++) {
            struct Decl** decl_p = (struct Decl**)vec_get(scope->decls, i);
            if (decl_p && *decl_p) visit(s, *decl_p);
        }
    }

    if (scope->children) {
        for (size_t i = 0; i < scope->children->count; i++) {
            struct Scope** child_p = (struct Scope**)vec_get(scope->children, i);
            if (child_p && *child_p) walk_scope_decls(s, *child_p, visit);
        }
    }
}

static void allocate_skeleton_visit(struct Sema* s, struct Decl* decl) {
    allocate_skeleton(s, decl);
}

static void resolve_signature_visit(struct Sema* s, struct Decl* decl) {
    sema_signature_of_decl(s, decl);
}

static bool decl_signature_deferred(struct Sema* s, struct Decl* decl) {
    if (!decl || decl->semantic_kind != SEM_VALUE) return false;
    if (decl->kind == DECL_PARAM || decl->kind == DECL_PRIMITIVE || decl->kind == DECL_IMPORT) return false;
    if (decl_is_function_like(decl)) return false;
    if (decl->node && decl->node->kind == expr_Bind && decl->node->bind.type_ann) return false;
    return sema_decl_type(s, decl) == NULL;
}

static void resolve_struct_field(struct Sema* s, struct Decl* owner, struct FieldDef* field) {
    if (!field) return;

    struct Type* field_type = field->type
        ? sema_infer_type_expr(s, field->type)
        : s->unknown_type;
    struct Decl* field_decl = scope_lookup_member(owner ? owner->child_scope : NULL,
        field->name.string_id);
    if (field_decl && !sema_type_is_errorish(field_type)) {
        assign_decl_type(s, field_decl, field_type);
    }
}

static void resolve_struct_signature(struct Sema* s, struct Decl* decl) {
    struct Expr* value = decl_bind_value(decl);
    if (!value || value->kind != expr_Struct || !value->struct_expr.members) return;

    for (size_t i = 0; i < value->struct_expr.members->count; i++) {
        struct StructMember* member = (struct StructMember*)vec_get(value->struct_expr.members, i);
        if (!member) continue;
        if (member->kind == member_Field) {
            resolve_struct_field(s, decl, &member->field);
        } else if (member->kind == member_Union && member->union_def.variants) {
            for (size_t j = 0; j < member->union_def.variants->count; j++) {
                resolve_struct_field(s, decl,
                    (struct FieldDef*)vec_get(member->union_def.variants, j));
            }
        }
    }
}

static void resolve_enum_signature(struct Sema* s, struct Decl* decl) {
    struct Expr* value = decl_bind_value(decl);
    if (!value || value->kind != expr_Enum || !value->enum_expr.variants) return;

    struct Type* enum_type = sema_decl_type(s, decl);
    for (size_t i = 0; i < value->enum_expr.variants->count; i++) {
        struct EnumVariant* variant = (struct EnumVariant*)vec_get(value->enum_expr.variants, i);
        if (!variant) continue;
        if (variant->explicit_value) sema_infer_expr(s, variant->explicit_value);
        struct Decl* variant_decl = scope_lookup_member(decl->child_scope, variant->name.string_id);
        if (variant_decl && enum_type) assign_decl_type(s, variant_decl, enum_type);
    }
}

static void set_param_decl_type(struct Sema* s, struct Decl* owner, struct Param* param, struct Type* type) {
    if (!owner || !owner->child_scope || !param) return;
    struct Decl* param_decl = scope_lookup_member(owner->child_scope, param->name.string_id);
    if (param_decl) assign_decl_type(s, param_decl, type);
}

static void resolve_function_signature_from_parts(struct Sema* s, struct Decl* decl,
    Vec* params, struct Expr* ret_type, struct Expr* effect) {
    struct SemaDeclInfo* info = sema_decl_info(s, decl);
    if (!info) return;
    if (!info->type || info->type->kind != TYPE_FUNCTION) {
        info->type = sema_function_type(s);
        if (info->type) info->type->decl = decl;
    }
    if (!info->type) return;

    if (params && info->type->params && info->type->params->count == 0) {
        for (size_t i = 0; i < params->count; i++) {
            struct Param* param = (struct Param*)vec_get(params, i);
            struct Type* param_type = param && param->type_ann
                ? sema_infer_type_expr(s, param->type_ann)
                : s->unknown_type;
            vec_push(info->type->params, &param_type);
            if (param) set_param_decl_type(s, decl, param, param_type);
        }
    }

    if (effect) {
        struct EffectSig* sig = sema_effect_sig_from_expr(s, effect);
        info->effect_sig = sig;
        info->type->effect_sig = sig;
        if (sig && info->type->effects && info->type->effects->count == 0) {
            vec_push(info->type->effects, &sig);
        }
        sema_infer_expr(s, effect);
    }

    if (!info->type->ret || sema_type_is_errorish(info->type->ret)) {
        info->type->ret = ret_type ? sema_infer_type_expr(s, ret_type) : s->unknown_type;
    }
}

static void resolve_function_signature(struct Sema* s, struct Decl* decl) {
    if (!decl || !decl->node) return;

    if (decl->node->kind == expr_Lambda) {
        resolve_function_signature_from_parts(s, decl, decl->node->lambda.params,
            decl->node->lambda.ret_type, decl->node->lambda.effect);
        return;
    }
    if (decl->node->kind == expr_Ctl) {
        resolve_function_signature_from_parts(s, decl, decl->node->ctl.params,
            decl->node->ctl.ret_type, NULL);
        return;
    }

    struct Expr* value = decl_bind_value(decl);
    if (!value) return;
    if (value->kind == expr_Lambda) {
        resolve_function_signature_from_parts(s, decl, value->lambda.params,
            value->lambda.ret_type, value->lambda.effect);
    } else if (value->kind == expr_Ctl) {
        resolve_function_signature_from_parts(s, decl, value->ctl.params,
            value->ctl.ret_type, NULL);
    }
}

static void resolve_effect_signature(struct Sema* s, struct Decl* decl) {
    struct Expr* value = decl_bind_value(decl);
    if (!value || value->kind != expr_Effect || !value->effect_expr.operations) return;

    for (size_t i = 0; i < value->effect_expr.operations->count; i++) {
        struct Expr** op_p = (struct Expr**)vec_get(value->effect_expr.operations, i);
        struct Expr* op = op_p ? *op_p : NULL;
        if (!op || op->kind != expr_Bind) continue;

        struct Decl* op_decl = scope_lookup_member(decl->child_scope, op->bind.name.string_id);
        if (op_decl) sema_signature_of_decl(s, op_decl);
    }
}

static void report_decl_cycle(struct Sema* s, struct Decl* decl) {
    if (!s || !decl) return;
    struct SemaDeclInfo* info = sema_decl_info(s, decl);
    if (!info || info->type_query.state == QUERY_ERROR) return;
    const char* name = s->pool ? pool_get(s->pool, decl->name.string_id, 0) : NULL;
    sema_error(s, decl->name.span, "circular definition of '%s'", name ? name : "?");
    info->type_query.state = QUERY_ERROR;
    info->type = s->error_type;
}

static struct Type* compute_decl_signature(struct Sema* s, struct Decl* decl) {
    if (!decl) return s->error_type;
    struct SemaDeclInfo* info = sema_decl_info(s, decl);
    if (!info) return s->error_type;

    if (!s || !decl) return s ? s->unknown_type : NULL;
    if (!info) return s->unknown_type;

    if (decl_signature_deferred(s, decl)) {
        try_fold_decl_value(s, decl, info);
        return info->type ? info->type : s->unknown_type;
    }

    QueryBeginResult begin = sema_query_begin(s, &info->type_query,
        QUERY_TYPE_OF_DECL, decl, decl->name.span);
    switch (begin) {
        case QUERY_BEGIN_CACHED:
            return info->type ? info->type : s->unknown_type;
        case QUERY_BEGIN_ERROR:
            return s->error_type;
        case QUERY_BEGIN_CYCLE:
            if (decl->semantic_kind == SEM_TYPE && info->type) return info->type;
            report_decl_cycle(s, decl);
            return s->error_type;
        case QUERY_BEGIN_COMPUTE:
            break;
    }

    allocate_skeleton(s, decl);

    switch (decl->semantic_kind) {
        case SEM_MODULE:
        case SEM_SCOPE_TOKEN:
        case SEM_EFFECT_ROW:
            break;
        case SEM_EFFECT:
            resolve_effect_signature(s, decl);
            break;
        case SEM_TYPE:
            if (info->type && info->type->kind == TYPE_STRUCT) {
                resolve_struct_signature(s, decl);
            } else if (info->type && info->type->kind == TYPE_ENUM) {
                resolve_enum_signature(s, decl);
            }
            break;
        case SEM_VALUE:
            if (decl->kind == DECL_PARAM) {
                struct Param* param = find_param_decl(decl);
                info->type = param && param->type_ann
                    ? sema_infer_type_expr(s, param->type_ann)
                    : s->unknown_type;
            } else if (decl_is_function_like(decl)) {
                resolve_function_signature(s, decl);
            } else if (decl->node && decl->node->kind == expr_Bind && decl->node->bind.type_ann) {
                info->type = sema_infer_type_expr(s, decl->node->bind.type_ann);
            }
            break;
        case SEM_UNKNOWN:
        default:
            break;
    }

    if (info->type_query.state == QUERY_ERROR) {
        sema_query_fail(s, &info->type_query);
        return s->error_type;
    }
    sema_query_succeed(s, &info->type_query);

    // Function-like decls: once the signature is in, infer body effects and
    // verify they fit the declared signature. Generics are deferred — their
    // body is checked per Instantiation, not here.
    if (decl->semantic_kind == SEM_VALUE && info->type &&
        info->type->kind == TYPE_FUNCTION && decl_is_function_like(decl)) {
        bool is_generic = false;
        Vec* params = NULL;
        if (decl->node) {
            if (decl->node->kind == expr_Lambda) params = decl->node->lambda.params;
            else if (decl->node->kind == expr_Ctl) params = decl->node->ctl.params;
            else if (decl->node->kind == expr_Bind && decl->node->bind.value) {
                struct Expr* v = decl->node->bind.value;
                if (v->kind == expr_Lambda) params = v->lambda.params;
                else if (v->kind == expr_Ctl) params = v->ctl.params;
            }
        }
        if (params) {
            for (size_t i = 0; i < params->count; i++) {
                struct Param* p = (struct Param*)vec_get(params, i);
                if (p && p->kind != PARAM_RUNTIME) { is_generic = true; break; }
            }
        }
        // Skip the sig-vs-body effect check for op-implementation decls
        // nested inside a `with handler` block: their declared signature is
        // the *effect's* signature, and the implementation body is allowed
        // to perform any effect of the enclosing function.
        bool is_handler_impl = s->compiler && hashmap_contains(
            &s->compiler->handler_impl_decls, (uint64_t)(uintptr_t)decl);
        if (!is_generic && !is_handler_impl) {
            struct EffectSet* inferred = sema_body_effects_of(s, decl);
            if (inferred) sema_solve_effect_rows(s, decl, info->effect_sig, inferred);
        }
    }

    try_fold_decl_value(s, decl, info);
    return info->type;
}

bool sema_collect_declarations(struct Sema* s) {
    if (!s || !s->resolver) return false;

    Vec* modules = s->compiler ? s->compiler->modules : NULL;
    if (!modules) return true;

    for (size_t i = 0; i < modules->count; i++) {
        struct Module** mod_p = (struct Module**)vec_get(modules, i);
        struct Module* mod = mod_p ? *mod_p : NULL;
        if (mod && mod->scope) walk_scope_decls(s, mod->scope, allocate_skeleton_visit);
    }

    for (size_t i = 0; i < modules->count; i++) {
        struct Module** mod_p = (struct Module**)vec_get(modules, i);
        struct Module* mod = mod_p ? *mod_p : NULL;
        if (mod && mod->scope) walk_scope_decls(s, mod->scope, resolve_signature_visit);
    }

    return !s->has_errors;
}

struct Type* sema_signature_of_decl(struct Sema* s, struct Decl* decl) {
    if (!s) return NULL;
    if (!decl) return s->unknown_type;
    struct SemaDeclInfo* info = sema_decl_info(s, decl);
    if (!info) return s->unknown_type;
    if (info->type_query.state == QUERY_ERROR) return s->error_type;
    if (info->type_query.state == QUERY_RUNNING) {
        if (decl->semantic_kind == SEM_TYPE && info->type) return info->type;
        report_decl_cycle(s, decl);
        return s->error_type;
    }
    if (info->type && info->type_query.state == QUERY_DONE) return info->type;
    return compute_decl_signature(s, decl);
}

struct Type* sema_type_of_decl(struct Sema* s, struct Decl* decl) {
    return sema_signature_of_decl(s, decl);
}

struct Type* sema_type_from_decl(struct Sema* s, struct Decl* decl) {
    return sema_type_of_decl(s, decl);
}
