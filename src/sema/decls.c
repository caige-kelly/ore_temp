#include "decls.h"

#include "checker.h"
#include "type.h"
#include "sema.h"
#include "./compiler/compiler.h" 

bool sema_collect_declarations(struct Sema* s) {
    if (!s || !s->resolver) return false;

    Vec* modules = s->compiler ? s->compiler->modules : NULL;
    if (!modules) return true;

    for (size_t i = 0; i < modules->count; i++) {
        struct Module** mod_p = (struct Module**)vec_get(modules, i);
        struct Module* mod = mod_p ? *mod_p : NULL;
        if (!mod || !mod->ast) continue;

        for (size_t j = 0; j < mod->ast->count; j++) {
            struct Expr** expr_p = (struct Expr**)vec_get(mod->ast, j);
            struct Expr* expr = expr_p ? *expr_p : NULL;
            if (!expr || expr-> kind != expr_Bind) continue;
            if (!expr->bind.name.resolved) continue;

            struct Decl* decl = expr->bind.name.resolved;
            struct Type* t = NULL;
            if (expr->bind.type_ann) {
                t = sema_infer_type_expr(s, expr->bind.type_ann);
            } else if (expr->bind.value) {
                t = sema_infer_expr(s, expr->bind.value);
            }
            if (t) decl->cached_type = t;
        }
    }
    return true;
}

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

struct Type* sema_type_from_decl(struct Sema* s, struct Decl* decl) {
    if (!decl) return s->unknown_type;

    if (decl->cached_type && !sema_type_is_errorish(decl->cached_type)) {
        return decl->cached_type;
    }

    switch (decl->semantic_kind) {
        case SEM_MODULE:
            return sema_named_type(s, TYPE_MODULE, decl->name.string_id, decl);
        case SEM_EFFECT:
            return sema_named_type(s, TYPE_EFFECT, decl->name.string_id, decl);
        case SEM_SCOPE_TOKEN: {
            struct Type* type = sema_named_type(s, TYPE_SCOPE_TOKEN, decl->name.string_id, decl);
            type->region_id = decl->scope_token_id;
            return type;
        }
        case SEM_EFFECT_ROW:
            return sema_named_type(s, TYPE_EFFECT_ROW, decl->name.string_id, decl);
        case SEM_VALUE:
            if (decl->kind == DECL_PARAM) {
                struct Param* param = find_param_decl(decl);
                return param && param->type_ann
                    ? sema_infer_type_expr(s, param->type_ann)
                    : s->unknown_type;
            }
            if (decl->kind == DECL_FIELD && decl->node && decl->node->kind == expr_Bind &&
                decl->node->bind.value && decl->node->bind.value->kind == expr_Lambda) {
                return sema_infer_expr(s, decl->node->bind.value);
            }
            if (decl->kind == DECL_PRIMITIVE) return sema_primitive_type_for_name(s, decl->name.string_id);
            if (decl->node && decl->node->kind == expr_Bind) {
                if (decl->node->bind.type_ann) return sema_infer_type_expr(s, decl->node->bind.type_ann);
                if (decl->node->bind.value && decl->node->bind.value->kind == expr_Lambda) {
                    return sema_infer_expr(s, decl->node->bind.value);
                }
            }
            return s->unknown_type;
        case SEM_TYPE:
            if (decl->kind == DECL_PRIMITIVE) return sema_primitive_type_for_name(s, decl->name.string_id);
            if (decl->node && decl->node->kind == expr_Bind && decl->node->bind.value) {
                switch (decl->node->bind.value->kind) {
                    case expr_Struct: return sema_named_type(s, TYPE_STRUCT, decl->name.string_id, decl);
                    case expr_Enum:   return sema_named_type(s, TYPE_ENUM, decl->name.string_id, decl);
                    default: break;
                }
            }
            return sema_named_type(s, TYPE_TYPE, decl->name.string_id, decl);
        case SEM_UNKNOWN:
        default:
            return s->unknown_type;
    }
}