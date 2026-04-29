#include "checker.h"

#include "decls.h"
#include "effects.h"
#include "sema_internal.h"
#include "type.h"
#include "../compiler/compiler.h"

static void report_type_mismatch(struct Sema* s, struct Span span,
    struct Type* expected, struct Type* actual) {
    char expected_name[128];
    char actual_name[128];
    sema_error(s, span, "expected %s but found %s",
        sema_type_display_name(s, expected, expected_name, sizeof(expected_name)),
        sema_type_display_name(s, actual, actual_name, sizeof(actual_name)));
}

static void report_expected_type_expr(struct Sema* s, struct Expr* expr, struct Type* actual) {
    if (!expr || sema_type_is_errorish(actual)) return;
    char actual_name[128];
    sema_error(s, expr->span, "expected type expression but found %s",
        sema_type_display_name(s, actual, actual_name, sizeof(actual_name)));
}

static const char* type_display_name(struct Sema* s, struct Type* type, char* buffer, size_t buffer_size) {
    return sema_type_display_name(s, type, buffer, buffer_size);
}

static struct Type* infer_lit(struct Sema* s, struct Expr* expr) {
    switch (expr->lit.kind) {
        case lit_Int:    return s->comptime_int_type;
        case lit_Float:  return s->comptime_float_type;
        case lit_String: return s->string_type;
        case lit_Byte:   return s->u8_type;
        case lit_True:
        case lit_False:  return s->bool_type;
        case lit_Nil:    return s->nil_type;
    }
    return s->unknown_type;
}

static bool op_is_comparison(enum TokenKind op) {
    switch (op) {
        case EqualEqual:
        case BangEqual:
        case Less:
        case LessEqual:
        case Greater:
        case GreaterEqual:
            return true;
        default:
            return false;
    }
}

static struct Type* infer_function_like(struct Sema* s, Vec* params,
    struct Expr* ret_type, struct Expr* effect, struct Expr* body) {
    struct Type* fn = sema_function_type(s);
    if (params) {
        for (size_t i = 0; i < params->count; i++) {
            struct Param* param = (struct Param*)vec_get(params, i);
            struct Type* param_type = param ? sema_infer_type_expr(s, param->type_ann) : s->unknown_type;
            vec_push(fn->params, &param_type);
        }
    }
    if (effect) {
        struct EffectSig* sig = sema_effect_sig_from_expr(s, effect);
        fn->effect_sig = sig;
        if (sig) vec_push(fn->effects, &sig);
        sema_infer_expr(s, effect);
    }
    fn->ret = ret_type ? sema_infer_type_expr(s, ret_type) : sema_infer_expr(s, body);
    if (ret_type && body) {
        if (fn->ret && fn->ret->kind == TYPE_VOID) {
            sema_infer_expr(s, body);
        } else {
            sema_check_expr(s, body, fn->ret);
        }
    }
    return fn;
}

static struct FieldDef* struct_field_by_name(struct Type* type, uint32_t name_id) {
    if (!type || type->kind != TYPE_STRUCT || !type->decl || !type->decl->node) return NULL;
    if (type->decl->node->kind != expr_Bind || !type->decl->node->bind.value) return NULL;
    struct Expr* value = type->decl->node->bind.value;
    if (value->kind != expr_Struct || !value->struct_expr.members) return NULL;

    for (size_t i = 0; i < value->struct_expr.members->count; i++) {
        struct StructMember* member = (struct StructMember*)vec_get(value->struct_expr.members, i);
        if (!member) continue;
        if (member->kind == member_Field && member->field.name.string_id == name_id) {
            return &member->field;
        }
    }
    return NULL;
}

static bool product_has_field(struct ProductExpr* product, uint32_t name_id) {
    if (!product || !product->Fields) return false;
    for (size_t i = 0; i < product->Fields->count; i++) {
        struct ProductField* field = (struct ProductField*)vec_get(product->Fields, i);
        if (field && field->name.string_id == name_id) return true;
    }
    return false;
}

static bool product_field_seen_before(struct ProductExpr* product, size_t index, uint32_t name_id) {
    if (!product || !product->Fields || name_id == 0) return false;
    for (size_t i = 0; i < index; i++) {
        struct ProductField* field = (struct ProductField*)vec_get(product->Fields, i);
        if (field && field->name.string_id == name_id) return true;
    }
    return false;
}

static bool check_product_literal(struct Sema* s, struct Expr* expr, struct Type* expected) {
    if (!expr || expr->kind != expr_Product || !expected || expected->kind != TYPE_STRUCT) return false;
    bool ok = true;
    struct ProductExpr* product = &expr->product;

    if (product->Fields) {
        for (size_t i = 0; i < product->Fields->count; i++) {
            struct ProductField* field = (struct ProductField*)vec_get(product->Fields, i);
            if (!field) continue;
            if (field->is_spread || field->name.string_id == 0) {
                sema_infer_expr(s, field->value);
                continue;
            }

            if (product_field_seen_before(product, i, field->name.string_id)) {
                const char* field_name = pool_get(s->pool, field->name.string_id, 0);
                sema_error(s, field->name.span, "duplicate field '%s' in product literal",
                    field_name ? field_name : "?");
                ok = false;
                continue;
            }

            struct FieldDef* def = struct_field_by_name(expected, field->name.string_id);
            if (!def) {
                char type_name[128];
                const char* field_name = pool_get(s->pool, field->name.string_id, 0);
                sema_error(s, field->name.span, "struct '%s' has no field '%s'",
                    type_display_name(s, expected, type_name, sizeof(type_name)),
                    field_name ? field_name : "?");
                sema_infer_expr(s, field->value);
                ok = false;
                continue;
            }

            struct Type* field_type = sema_infer_type_expr(s, def->type);
            if (!sema_check_expr(s, field->value, field_type)) ok = false;
        }
    }

    if (expected->decl && expected->decl->node && expected->decl->node->kind == expr_Bind &&
        expected->decl->node->bind.value && expected->decl->node->bind.value->kind == expr_Struct) {
        struct StructExpr* st = &expected->decl->node->bind.value->struct_expr;
        if (st->members) {
            for (size_t i = 0; i < st->members->count; i++) {
                struct StructMember* member = (struct StructMember*)vec_get(st->members, i);
                if (!member || member->kind != member_Field) continue;
                if (member->field.default_value) continue;
                if (!product_has_field(product, member->field.name.string_id)) {
                    char type_name[128];
                    const char* field_name = pool_get(s->pool, member->field.name.string_id, 0);
                    sema_error(s, expr->span, "missing field '%s' for struct '%s'",
                        field_name ? field_name : "?",
                        type_display_name(s, expected, type_name, sizeof(type_name)));
                    ok = false;
                }
            }
        }
    }

    sema_record_fact(s, expr, expected, SEM_VALUE, expected->region_id);
    return ok;
}

static bool check_block_expr(struct Sema* s, struct Expr* expr, struct Type* expected) {
    if (!expr || expr->kind != expr_Block) return false;
    Vec* stmts = &expr->block.stmts;
    if (stmts->count == 0) {
        struct Type* actual = s->void_type;
        if (!sema_type_assignable(expected, actual)) {
            report_type_mismatch(s, expr->span, expected, actual);
            return false;
        }
        sema_record_fact(s, expr, actual, SEM_VALUE, 0);
        return true;
    }

    for (size_t i = 0; i < stmts->count; i++) {
        struct Expr** stmt = (struct Expr**)vec_get(stmts, i);
        if (!stmt || !*stmt) continue;
        if (i + 1 == stmts->count) {
            bool ok = sema_check_expr(s, *stmt, expected);
            sema_record_fact(s, expr, expected, SEM_VALUE, expected ? expected->region_id : 0);
            return ok;
        }
        sema_infer_expr(s, *stmt);
    }
    return true;
}

static bool check_if_expr(struct Sema* s, struct Expr* expr, struct Type* expected) {
    if (!expr || expr->kind != expr_If) return false;
    sema_check_expr(s, expr->if_expr.condition, s->bool_type);
    bool ok = sema_check_expr(s, expr->if_expr.then_branch, expected);
    if (expr->if_expr.else_branch) {
        if (!sema_check_expr(s, expr->if_expr.else_branch, expected)) ok = false;
    } else if (expected && expected->kind != TYPE_VOID && !sema_type_is_errorish(expected)) {
        char expected_name[128];
        sema_error(s, expr->span, "if expression missing else branch for expected %s",
            type_display_name(s, expected, expected_name, sizeof(expected_name)));
        ok = false;
    }
    sema_record_fact(s, expr, expected, SEM_VALUE, expected ? expected->region_id : 0);
    return ok;
}

static void check_call_args(struct Sema* s, struct Expr* expr, struct Type* callee) {
    if (!expr || expr->kind != expr_Call || !callee || callee->kind != TYPE_FUNCTION) return;

    size_t expected_count = callee->params ? callee->params->count : 0;
    size_t actual_count = expr->call.args ? expr->call.args->count : 0;
    if (expected_count != actual_count) {
        sema_error(s, expr->span, "expected %zu arguments but found %zu",
            expected_count, actual_count);
    }

    size_t check_count = expected_count < actual_count ? expected_count : actual_count;
    for (size_t i = 0; i < check_count; i++) {
        struct Type** param_type = (struct Type**)vec_get(callee->params, i);
        struct Expr** arg = (struct Expr**)vec_get(expr->call.args, i);
        if (param_type && arg && *arg) sema_check_expr(s, *arg, *param_type);
    }

    for (size_t i = check_count; i < actual_count; i++) {
        struct Expr** arg = (struct Expr**)vec_get(expr->call.args, i);
        if (arg && *arg) sema_infer_expr(s, *arg);
    }
}

struct Type* sema_infer_expr(struct Sema* s, struct Expr* expr) {
    if (!expr) return s->void_type;

    struct Type* result = s->unknown_type;
    SemanticKind semantic = SEM_UNKNOWN;
    uint32_t region_id = 0;

    switch (expr->kind) {
        case expr_Lit:
            result = infer_lit(s, expr);
            semantic = SEM_VALUE;
            break;
        case expr_Ident: {
            struct Decl* decl = expr->ident.resolved;
            result = sema_type_from_decl(s, decl);
            semantic = decl ? decl->semantic_kind : SEM_UNKNOWN;
            region_id = result ? result->region_id : 0;
            break;
        }
        case expr_Field: {
            sema_infer_expr(s, expr->field.object);
            struct Decl* decl = expr->field.field.resolved;
            result = sema_type_from_decl(s, decl);
            semantic = decl ? decl->semantic_kind : sema_semantic_for_type(result);
            region_id = result ? result->region_id : 0;
            break;
        }
        case expr_Builtin:
            if (sema_name_is(s, expr->builtin.name_id, "import")) {
                result = s->module_type;
                semantic = SEM_MODULE;
            } else if (sema_name_is(s, expr->builtin.name_id, "sizeOf") ||
                sema_name_is(s, expr->builtin.name_id, "alignOf") ||
                sema_name_is(s, expr->builtin.name_id, "intCast")) {
                result = s->comptime_int_type;
                semantic = SEM_VALUE;
            } else if (sema_name_is(s, expr->builtin.name_id, "TypeOf")) {
                result = s->type_type;
                semantic = SEM_TYPE;
            } else {
                result = s->unknown_type;
                semantic = SEM_VALUE;
            }
            if (expr->builtin.args) {
                for (size_t i = 0; i < expr->builtin.args->count; i++) {
                    struct Expr** arg = (struct Expr**)vec_get(expr->builtin.args, i);
                    if (arg) sema_infer_expr(s, *arg);
                }
            }
            break;
        case expr_Bin: {
            struct Type* left = sema_infer_expr(s, expr->bin.Left);
            sema_infer_expr(s, expr->bin.Right);
            result = op_is_comparison(expr->bin.op) ? s->bool_type : left;
            semantic = SEM_VALUE;
            break;
        }
        case expr_Assign:
            sema_infer_expr(s, expr->assign.target);
            result = sema_infer_expr(s, expr->assign.value);
            semantic = SEM_VALUE;
            break;
        case expr_Unary: {
            struct Type* inner = sema_infer_expr(s, expr->unary.operand);
            if (expr->unary.op == unary_Ref || expr->unary.op == unary_Ptr) {
                result = sema_pointer_type(s, inner);
                region_id = result->region_id;
            } else {
                result = inner;
            }
            semantic = SEM_VALUE;
            break;
        }
        case expr_Call: {
            struct Type* callee = sema_infer_expr(s, expr->call.callee);
            if (callee && callee->kind == TYPE_FUNCTION && callee->ret) {
                check_call_args(s, expr, callee);
                result = callee->ret;
            } else if (callee && callee->kind == TYPE_EFFECT) {
                if (expr->call.args) {
                    for (size_t i = 0; i < expr->call.args->count; i++) {
                        struct Expr** arg = (struct Expr**)vec_get(expr->call.args, i);
                        if (arg) sema_infer_expr(s, *arg);
                    }
                }
                result = callee;
                semantic = SEM_EFFECT;
                break;
            } else {
                if (expr->call.args) {
                    for (size_t i = 0; i < expr->call.args->count; i++) {
                        struct Expr** arg = (struct Expr**)vec_get(expr->call.args, i);
                        if (arg) sema_infer_expr(s, *arg);
                    }
                }
                if (!sema_type_is_errorish(callee)) {
                    char callee_name[128];
                    sema_error(s, expr->span, "cannot call value of type %s",
                        type_display_name(s, callee, callee_name, sizeof(callee_name)));
                }
                result = s->unknown_type;
            }
            semantic = SEM_VALUE;
            break;
        }
        case expr_EffectRow:
            sema_infer_expr(s, expr->effect_row.head);
            if (expr->effect_row.row.resolved &&
                expr->effect_row.row.resolved->semantic_kind != SEM_EFFECT_ROW) {
                const char* name = pool_get(s->pool, expr->effect_row.row.string_id, 0);
                sema_error(s, expr->effect_row.row.span,
                    "'%s' is not an effect-row variable", name ? name : "?");
            }
            result = s->effect_row_type;
            semantic = SEM_EFFECT_ROW;
            break;
        case expr_Lambda:
            result = infer_function_like(s, expr->lambda.params, expr->lambda.ret_type,
                expr->lambda.effect, expr->lambda.body);
            semantic = SEM_VALUE;
            break;
        case expr_Ctl:
            result = infer_function_like(s, expr->ctl.params, expr->ctl.ret_type, NULL, expr->ctl.body);
            semantic = SEM_VALUE;
            break;
        case expr_Struct:
            if (expr->struct_expr.members) {
                for (size_t i = 0; i < expr->struct_expr.members->count; i++) {
                    struct StructMember* member = (struct StructMember*)vec_get(expr->struct_expr.members, i);
                    if (!member) continue;
                    if (member->kind == member_Field) {
                        sema_infer_type_expr(s, member->field.type);
                        sema_infer_expr(s, member->field.default_value);
                    } else if (member->kind == member_Union && member->union_def.variants) {
                        for (size_t j = 0; j < member->union_def.variants->count; j++) {
                            struct FieldDef* field = (struct FieldDef*)vec_get(member->union_def.variants, j);
                            if (!field) continue;
                            sema_infer_type_expr(s, field->type);
                            sema_infer_expr(s, field->default_value);
                        }
                    }
                }
            }
            result = sema_type_new(s, TYPE_STRUCT);
            semantic = SEM_TYPE;
            break;
        case expr_Enum:
            if (expr->enum_expr.variants) {
                for (size_t i = 0; i < expr->enum_expr.variants->count; i++) {
                    struct EnumVariant* variant = (struct EnumVariant*)vec_get(expr->enum_expr.variants, i);
                    if (variant) sema_infer_expr(s, variant->explicit_value);
                }
            }
            result = sema_type_new(s, TYPE_ENUM);
            semantic = SEM_TYPE;
            break;
        case expr_Effect:
            if (expr->effect_expr.operations) {
                for (size_t i = 0; i < expr->effect_expr.operations->count; i++) {
                    struct Expr** op = (struct Expr**)vec_get(expr->effect_expr.operations, i);
                    if (op) sema_infer_expr(s, *op);
                }
            }
            result = s->effect_type;
            semantic = SEM_EFFECT;
            break;
        case expr_Bind:
            if (expr->bind.type_ann) {
                struct Type* expected = sema_infer_type_expr(s, expr->bind.type_ann);
                if (expr->bind.value) sema_check_expr(s, expr->bind.value, expected);
                result = expected;
            } else {
                result = sema_infer_expr(s, expr->bind.value);
            }
            semantic = sema_semantic_for_type(result);
            break;
        case expr_DestructureBind:
            result = sema_infer_expr(s, expr->destructure.value);
            semantic = SEM_VALUE;
            break;
        case expr_Block: {
            result = s->void_type;
            Vec* stmts = &expr->block.stmts;
            for (size_t i = 0; i < stmts->count; i++) {
                struct Expr** stmt = (struct Expr**)vec_get(stmts, i);
                if (stmt && *stmt) result = sema_infer_expr(s, *stmt);
            }
            semantic = SEM_VALUE;
            break;
        }
        case expr_Product:
            if (expr->product.type_expr) {
                struct Type* explicit_type = sema_infer_type_expr(s, expr->product.type_expr);
                if (explicit_type && explicit_type->kind == TYPE_STRUCT) {
                    check_product_literal(s, expr, explicit_type);
                    result = explicit_type;
                    semantic = SEM_VALUE;
                    break;
                }
            }
            if (expr->product.Fields) {
                for (size_t i = 0; i < expr->product.Fields->count; i++) {
                    struct ProductField* field = (struct ProductField*)vec_get(expr->product.Fields, i);
                    if (field) sema_infer_expr(s, field->value);
                }
            }
            result = sema_type_new(s, TYPE_PRODUCT);
            semantic = SEM_VALUE;
            break;
        case expr_If:
            sema_infer_expr(s, expr->if_expr.condition);
            result = sema_infer_expr(s, expr->if_expr.then_branch);
            sema_infer_expr(s, expr->if_expr.else_branch);
            semantic = SEM_VALUE;
            break;
        case expr_Switch:
            sema_infer_expr(s, expr->switch_expr.scrutinee);
            if (expr->switch_expr.arms) {
                for (size_t i = 0; i < expr->switch_expr.arms->count; i++) {
                    struct SwitchArm* arm = (struct SwitchArm*)vec_get(expr->switch_expr.arms, i);
                    if (!arm) continue;
                    if (arm->patterns) {
                        for (size_t j = 0; j < arm->patterns->count; j++) {
                            struct Expr** pat = (struct Expr**)vec_get(arm->patterns, j);
                            if (pat) sema_infer_expr(s, *pat);
                        }
                    }
                    result = sema_infer_expr(s, arm->body);
                }
            }
            semantic = SEM_VALUE;
            break;
        case expr_With:
            sema_infer_expr(s, expr->with.func);
            result = sema_infer_expr(s, expr->with.body);
            semantic = SEM_VALUE;
            break;
        case expr_Index:
            sema_infer_expr(s, expr->index.object);
            sema_infer_expr(s, expr->index.index);
            result = s->unknown_type;
            semantic = SEM_VALUE;
            break;
        case expr_Loop:
            sema_infer_expr(s, expr->loop_expr.init);
            sema_infer_expr(s, expr->loop_expr.condition);
            sema_infer_expr(s, expr->loop_expr.step);
            result = sema_infer_expr(s, expr->loop_expr.body);
            semantic = SEM_VALUE;
            break;
        case expr_Return:
            result = sema_infer_expr(s, expr->return_expr.value);
            semantic = SEM_VALUE;
            break;
        case expr_Defer:
            sema_infer_expr(s, expr->defer_expr.value);
            result = s->void_type;
            semantic = SEM_VALUE;
            break;
        case expr_ArrayType:
            sema_infer_expr(s, expr->array_type.size);
            if (expr->array_type.is_many_ptr) {
                result = sema_pointer_type(s, sema_infer_type_expr(s, expr->array_type.elem));
            } else {
                result = sema_array_type(s, sema_infer_type_expr(s, expr->array_type.elem));
            }
            semantic = SEM_TYPE;
            break;
        case expr_SliceType:
            result = sema_slice_type(s, sema_infer_type_expr(s, expr->slice_type.elem));
            semantic = SEM_TYPE;
            break;
        case expr_ManyPtrType:
            result = sema_pointer_type(s, sema_infer_type_expr(s, expr->many_ptr_type.elem));
            semantic = SEM_TYPE;
            break;
        case expr_ArrayLit:
        {
            sema_infer_expr(s, expr->array_lit.size);
            struct Type* elem_type = sema_infer_type_expr(s, expr->array_lit.elem_type);
            sema_infer_expr(s, expr->array_lit.initializer);
            result = sema_array_type(s, elem_type);
            semantic = SEM_VALUE;
            break;
        }
        case expr_EnumRef:
            result = s->unknown_type;
            semantic = SEM_VALUE;
            break;
        case expr_Asm:
            result = s->unknown_type;
            semantic = SEM_VALUE;
            break;
        case expr_Break:
        case expr_Continue:
            result = s->void_type;
            semantic = SEM_VALUE;
            break;
    }

    if (region_id == 0 && result) region_id = result->region_id;
    if (semantic == SEM_UNKNOWN) semantic = sema_semantic_for_type(result);
    sema_record_fact(s, expr, result, semantic, region_id);
    return result;
}

struct Type* sema_infer_type_expr(struct Sema* s, struct Expr* expr) {
    if (!expr) return s->unknown_type;

    switch (expr->kind) {
        case expr_Ident: {
            struct Decl* decl = expr->ident.resolved;
            if (!decl) return s->unknown_type;
            if (decl->semantic_kind == SEM_TYPE) return sema_type_from_decl(s, decl);

            struct Type* value_type = sema_type_from_decl(s, decl);
            if (decl->kind == DECL_PARAM &&
                (sema_type_is_errorish(value_type) || value_type->kind == TYPE_TYPE ||
                 value_type->kind == TYPE_ANYTYPE)) {
                return s->unknown_type;
            }
            report_expected_type_expr(s, expr, value_type);
            return s->unknown_type;
        }
        case expr_Field: {
            sema_infer_expr(s, expr->field.object);
            struct Decl* decl = expr->field.field.resolved;
            if (decl && decl->semantic_kind == SEM_TYPE) return sema_type_from_decl(s, decl);
            struct Type* value_type = sema_type_from_decl(s, decl);
            report_expected_type_expr(s, expr, value_type);
            return s->unknown_type;
        }
        case expr_Unary:
            switch (expr->unary.op) {
                case unary_Ptr:
                case unary_ManyPtr:
                    return sema_pointer_type(s, sema_infer_type_expr(s, expr->unary.operand));
                case unary_Const:
                case unary_Optional:
                    return sema_infer_type_expr(s, expr->unary.operand);
                default:
                    break;
            }
            break;
        case expr_ArrayType:
            sema_infer_expr(s, expr->array_type.size);
            return expr->array_type.is_many_ptr
                ? sema_pointer_type(s, sema_infer_type_expr(s, expr->array_type.elem))
                : sema_array_type(s, sema_infer_type_expr(s, expr->array_type.elem));
        case expr_SliceType:
            return sema_slice_type(s, sema_infer_type_expr(s, expr->slice_type.elem));
        case expr_ManyPtrType:
            return sema_pointer_type(s, sema_infer_type_expr(s, expr->many_ptr_type.elem));
        case expr_Lambda:
            return infer_function_like(s, expr->lambda.params, expr->lambda.ret_type,
                expr->lambda.effect, NULL);
        case expr_Ctl:
            return infer_function_like(s, expr->ctl.params, expr->ctl.ret_type, NULL, NULL);
        case expr_Product:
            if (expr->product.type_expr) return sema_infer_type_expr(s, expr->product.type_expr);
            return sema_type_new(s, TYPE_PRODUCT);
        case expr_Builtin: {
            struct Type* builtin_type = sema_infer_expr(s, expr);
            return sema_type_is_errorish(builtin_type) || sema_type_is_type_value(builtin_type)
                ? s->unknown_type
                : builtin_type;
        }
        default:
            break;
    }

    struct Type* actual = sema_infer_expr(s, expr);
    if (!sema_type_is_errorish(actual) && !sema_type_is_type_value(actual)) {
        report_expected_type_expr(s, expr, actual);
    }
    return sema_type_is_type_value(actual) ? actual : s->unknown_type;
}

bool sema_check_expr(struct Sema* s, struct Expr* expr, struct Type* expected) {
    if (!expr) {
        struct Type* actual = s->void_type;
        if (sema_type_assignable(expected, actual)) return true;
        report_type_mismatch(s, (struct Span){0}, expected, actual);
        return false;
    }

    if (expr->kind == expr_Block) return check_block_expr(s, expr, expected);
    if (expr->kind == expr_If) return check_if_expr(s, expr, expected);
    if (expr->kind == expr_Product && expected && expected->kind == TYPE_STRUCT) {
        return check_product_literal(s, expr, expected);
    }
    if (expected && expected->kind == TYPE_TYPE) {
        struct Type* type_value = sema_infer_type_expr(s, expr);
        sema_record_fact(s, expr, type_value, SEM_TYPE, type_value ? type_value->region_id : 0);
        return !sema_type_is_errorish(type_value);
    }

    struct Type* actual = sema_infer_expr(s, expr);
    if (sema_type_assignable(expected, actual)) return true;
    report_type_mismatch(s, expr->span, expected, actual);
    return false;
}

bool sema_check_expressions(struct Sema* s) {
    if (!s || !s->resolver) return false;

    if (s->compiler && s->compiler->modules) {
        for (size_t i = 0; i < s->compiler->modules->count; i++) {
            struct Module** mod_p = (struct Module**)vec_get(s->compiler->modules, i);
            if (!mod_p || !*mod_p || !(*mod_p)->ast) continue;
            Vec* ast = (*mod_p)->ast;
            for (size_t j = 0; j < ast->count; j++) {
                struct Expr** expr = (struct Expr**)vec_get(ast, j);
                if (expr && *expr) sema_infer_expr(s, *expr);
            }
        }
    } else if (s->resolver->ast) {
        for (size_t i = 0; i < s->resolver->ast->count; i++) {
            struct Expr** expr = (struct Expr**)vec_get(s->resolver->ast, i);
            if (expr && *expr) sema_infer_expr(s, *expr);
        }
    }

    return !s->has_errors;
}