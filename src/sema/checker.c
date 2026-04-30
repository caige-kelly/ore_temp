#include "checker.h"

#include "const_eval.h"
#include "decls.h"
#include "effects.h"
#include "evidence.h"
#include "instantiate.h"
#include "layout.h"
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

// static bool op_is_comparison(enum TokenKind op) {
//     switch (op) {
//         case EqualEqual:
//         case BangEqual:
//         case Less:
//         case LessEqual:
//         case Greater:
//         case GreaterEqual:
//             return true;
//         default:
//             return false;
//     }
// }

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

static bool expr_is_function_like(struct Expr* expr) {
    return expr && (expr->kind == expr_Lambda || expr->kind == expr_Ctl);
}

static void merge_function_body_type(struct Type* dst, struct Type* src) {
    if (!dst || !src || dst->kind != TYPE_FUNCTION || src->kind != TYPE_FUNCTION) return;
    if (dst->params && dst->params->count == 0 && src->params) dst->params = src->params;
    if (sema_type_is_errorish(dst->ret) && src->ret) dst->ret = src->ret;
    if (!dst->effect_sig && src->effect_sig) dst->effect_sig = src->effect_sig;
    if (dst->effects && dst->effects->count == 0 && src->effects) dst->effects = src->effects;
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

static struct Decl* call_resolved_decl(struct Expr* call_expr) {
    if (!call_expr || call_expr->kind != expr_Call) return NULL;
    struct Expr* c = call_expr->call.callee;
    if (!c) return NULL;
    if (c->kind == expr_Ident) return c->ident.resolved;
    if (c->kind == expr_Field) return c->field.field.resolved;
    return NULL;
}

// Per-position arg checking for generic calls. The specialized type's params
// vector excludes comptime params; this walker keeps two cursors so each
// runtime arg is matched against its specialized param type while comptime
// args are just inferred (their value was already const-folded by the caller).
static void check_generic_call_args(struct Sema* s, struct Expr* call_expr,
    struct Type* spec_type, struct Decl* generic) {
    Vec* generic_params = sema_decl_function_params(generic);
    Vec* args = call_expr->call.args;
    if (!generic_params || !args) return;

    size_t expected = generic_params->count;
    size_t actual = args->count;
    if (expected != actual) {
        sema_error(s, call_expr->span, "expected %zu arguments but found %zu",
            expected, actual);
    }

    size_t walk_count = expected < actual ? expected : actual;
    size_t runtime_i = 0;
    for (size_t i = 0; i < walk_count; i++) {
        struct Param* p = (struct Param*)vec_get(generic_params, i);
        struct Expr** arg_p = (struct Expr**)vec_get(args, i);
        struct Expr* arg = arg_p ? *arg_p : NULL;
        if (!arg) continue;

        if (p && p->is_comptime) {
            sema_infer_expr(s, arg);
            continue;
        }

        struct Type** pt = (spec_type && spec_type->params && runtime_i < spec_type->params->count)
            ? (struct Type**)vec_get(spec_type->params, runtime_i)
            : NULL;
        if (pt) sema_check_expr(s, arg, *pt);
        else sema_infer_expr(s, arg);
        runtime_i++;
    }

    for (size_t i = walk_count; i < actual; i++) {
        struct Expr** arg_p = (struct Expr**)vec_get(args, i);
        if (arg_p && *arg_p) sema_infer_expr(s, *arg_p);
    }
}

// On a generic call site: const-eval each comptime arg, instantiate the decl,
// and return the instantiation's specialized function type. Returns NULL if
// the callee is not a generic decl, or if any comptime arg failed to fold.
static struct Type* try_instantiate_call_site(struct Sema* s, struct Expr* call_expr) {
    struct Decl* callee = call_resolved_decl(call_expr);
    if (!callee || !sema_decl_is_generic(callee)) return NULL;

    Vec* params = sema_decl_function_params(callee);
    Vec* args = call_expr->call.args;
    if (!params) return NULL;
    if (!args && sema_decl_comptime_param_count(callee) > 0) return NULL;

    struct ComptimeArgTuple tuple = sema_arg_tuple_new(s);
    bool ok = true;
    size_t walk = params->count;
    if (args && args->count < walk) walk = args->count;

    for (size_t i = 0; i < walk; i++) {
        struct Param* p = (struct Param*)vec_get(params, i);
        if (!p || !p->is_comptime) continue;

        struct Expr** arg_p = (struct Expr**)vec_get(args, i);
        struct Expr* arg = arg_p ? *arg_p : NULL;
        if (!arg) { ok = false; continue; }

        struct ConstValue v = sema_const_eval_expr(s, arg, s->current_env);
        if (!sema_const_value_is_valid(v)) {
            const char* pname = s->pool ? pool_get(s->pool, p->name.string_id, 0) : "?";
            sema_error(s, arg->span,
                "comptime argument '%s' must be known at compile time",
                pname ? pname : "?");
            ok = false;
            continue;
        }
        sema_arg_tuple_push(&tuple, v);
    }

    if (!ok) return NULL;

    struct Instantiation* inst = sema_instantiate_decl(s, callee, tuple);
    return inst ? inst->specialized_type : NULL;
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
            result = sema_type_of_decl(s, decl);
            semantic = decl ? decl->semantic_kind : SEM_UNKNOWN;
            region_id = result ? result->region_id : 0;
            break;
        }
        case expr_Field: {
            sema_infer_expr(s, expr->field.object);
            struct Decl* decl = expr->field.field.resolved;
            result = sema_type_of_decl(s, decl);
            semantic = decl ? decl->semantic_kind : sema_semantic_for_type(result);
            region_id = result ? result->region_id : 0;
            break;
        }
        case expr_Builtin:
            if (sema_name_is(s, expr->builtin.name_id, "import")) {
                result = s->module_type;
                semantic = SEM_MODULE;
            } else if (sema_name_is(s, expr->builtin.name_id, "sizeOf") ||
                sema_name_is(s, expr->builtin.name_id, "alignOf")) {
                result = s->comptime_int_type;
                semantic = SEM_VALUE;
                bool is_size = sema_name_is(s, expr->builtin.name_id, "sizeOf");
                struct Expr* arg = NULL;
                if (expr->builtin.args && expr->builtin.args->count > 0) {
                    struct Expr** arg_p = (struct Expr**)vec_get(expr->builtin.args, 0);
                    arg = arg_p ? *arg_p : NULL;
                }
                if (!arg) {
                    sema_error(s, expr->span, "@%s requires a type argument",
                        is_size ? "sizeOf" : "alignOf");
                } else {
                    struct Type* arg_type = sema_infer_type_expr(s, arg);
                    if (expr->builtin.args && expr->builtin.args->count > 1) {
                        for (size_t i = 1; i < expr->builtin.args->count; i++) {
                            struct Expr** extra = (struct Expr**)vec_get(expr->builtin.args, i);
                            if (extra && *extra) sema_infer_expr(s, *extra);
                        }
                    }
                    if (!sema_type_is_errorish(arg_type)) {
                        struct TypeLayout layout = sema_layout_of_type_at(s, arg_type, expr->span);
                        if (!layout.complete &&
                            arg_type->layout_query.state != QUERY_ERROR) {
                            char name[128];
                            sema_error(s, expr->span,
                                "@%s of type '%s' is not yet computable",
                                is_size ? "sizeOf" : "alignOf",
                                sema_type_display_name(s, arg_type, name, sizeof(name)));
                        }
                    }
                }
            } else if (sema_name_is(s, expr->builtin.name_id, "intCast")) {
                result = s->comptime_int_type;
                semantic = SEM_VALUE;
                if (expr->builtin.args) {
                    for (size_t i = 0; i < expr->builtin.args->count; i++) {
                        struct Expr** arg = (struct Expr**)vec_get(expr->builtin.args, i);
                        if (arg) sema_infer_expr(s, *arg);
                    }
                }
            } else if (sema_name_is(s, expr->builtin.name_id, "TypeOf")) {
                result = s->type_type;
                semantic = SEM_TYPE;
                if (expr->builtin.args) {
                    for (size_t i = 0; i < expr->builtin.args->count; i++) {
                        struct Expr** arg = (struct Expr**)vec_get(expr->builtin.args, i);
                        if (arg) sema_infer_expr(s, *arg);
                    }
                }
            } else {
                result = s->unknown_type;
                semantic = SEM_VALUE;
                if (expr->builtin.args) {
                    for (size_t i = 0; i < expr->builtin.args->count; i++) {
                        struct Expr** arg = (struct Expr**)vec_get(expr->builtin.args, i);
                        if (arg) sema_infer_expr(s, *arg);
                    }
                }
            }
            break;
        case expr_Bin: {
            struct Type* left = sema_infer_expr(s, expr->bin.Left);
            struct Type* right = sema_infer_expr(s, expr->bin.Right);

            semantic = SEM_VALUE;
            switch (expr->bin.op) {
                case Plus: case Minus: case Star: case Percent: case ForwardSlash:  {
                    struct Type* joined = sema_numeric_join(s, left, right);
                    if (joined) {
                        result = joined;
                        region_id = result->region_id;
                    } else {
                        if (!sema_type_is_errorish(left) && !sema_type_is_errorish(right)) {
                            char left_name[128];
                            char right_name[128];
                            sema_error(s, expr->span, "operator '%s' cannot combine %s and %s",
                                token_kind_to_str(expr->bin.op),
                                sema_type_display_name(s, left, left_name, sizeof(left_name)),
                                sema_type_display_name(s, right, right_name, sizeof(right_name)));
                        }
                        result = s->unknown_type;
                    }
                    break;
                }
                case EqualEqual: case BangEqual: case Less: case LessEqual: case Greater: case GreaterEqual: {
                    if (left && right && left->kind == TYPE_BOOL && right->kind == TYPE_BOOL) {}
                    else if (!sema_numeric_join(s, left, right)) {
                        char left_name[128];
                        char right_name[128];
                        sema_error(s, expr->span, "cannot compare %s and %s",
                            sema_type_display_name(s, left, left_name, sizeof(left_name)),
                            sema_type_display_name(s, right, right_name, sizeof(right_name)));
                    }
                    result = s->bool_type;
                    break;
                }

                case Ampersand: case Pipe: case Caret: case ShiftLeft: case ShiftRight: {
                    if (!sema_type_is_integer(left) || !sema_type_is_integer(right)) {
                        char left_name[128];
                        char right_name[128];
                        sema_error(s, expr->span, "operate '%s' expects integer operands, found %s and %s",
                            token_kind_to_str(expr->bin.op),
                            sema_type_display_name(s, left, left_name, sizeof(left_name)),
                            sema_type_display_name(s, right, right_name, sizeof(right_name)));
                        result = s->unknown_type;
                    } else {
                        struct Type* joined = sema_numeric_join(s, left, right);
                        result = joined;
                    }
                    break;
                }

                case AmpersandAmpersand: case PipePipe: {
                    if (!sema_type_is_errorish(left) && left->kind != TYPE_BOOL) {
                        char left_name[128];
                        sema_error(s, expr->span, "operator '%s' expects bool, found %s",
                            token_kind_to_str(expr->bin.op),
                            sema_type_display_name(s, left, left_name, sizeof(left_name)));
                    }
                    if (!sema_type_is_errorish(right) && right->kind != TYPE_BOOL) {
                        char right_name[128];
                        sema_error(s, expr->span, "operator '%s' expects bool, found %s",
                            token_kind_to_str(expr->bin.op),
                            sema_type_display_name(s, right, right_name, sizeof(right_name)));
                    }
                    result = s->bool_type;
                    break;
                }

                default:
                    result = s->unknown_type;
                    break;
            }
            break;
        }

        case expr_Assign:
            sema_infer_expr(s, expr->assign.target);
            result = sema_infer_expr(s, expr->assign.value);
            semantic = SEM_VALUE;
            break;

        case expr_Unary: {
            struct Type* inner = sema_infer_expr(s, expr->unary.operand);
            semantic = SEM_VALUE;

            switch(expr->unary.op) {
                case unary_Ref:
                case unary_Ptr:
                case unary_ManyPtr:
                    result = sema_pointer_type(s, inner);
                    region_id = result->region_id;
                    break;
                case unary_Deref:
                    if (inner && inner->kind == TYPE_POINTER) {
                        result = inner->elem ? inner->elem : s->unknown_type;
                        region_id = result->region_id;
                    } else {
                        if (!sema_type_is_errorish(inner)) {
                            char inner_name[128];
                            sema_error(s, expr->span, "cannot dereference value of type %s",
                                sema_type_display_name(s, inner, inner_name, sizeof(inner_name)));
                        }
                        result = s->unknown_type;
                    }
                    break;
                case unary_Not:
                    if (!sema_type_is_errorish(inner) && inner->kind != TYPE_BOOL) {
                        char inner_name[128];
                        sema_error(s, expr->span, "operator '!' expects bool, found %s",
                            sema_type_display_name(s, inner, inner_name, sizeof(inner_name)));
                    }
                    result = s->bool_type;
                    break;
                case unary_Neg:
                    if (!sema_type_is_errorish(inner) && !sema_type_is_numeric(inner)) {
                        char inner_name[128];
                        sema_error(s, expr->span, "operator '-' expects numeric operand, found %s",
                            sema_type_display_name(s, inner, inner_name, sizeof(inner_name)));
                    }
                    result = s->bool_type;
                    break;
                case unary_BitNot:
                    if(!sema_type_is_errorish(inner) && !sema_type_is_integer(inner)) {
                        char inner_name[128];
                        sema_error(s, expr->span, "operator '~' expects integer operand, found %s",
                            sema_type_display_name(s, inner, inner_name, sizeof(inner_name)));
                        result = s->unknown_type;
                    } else {
                        result = inner;
                    }
                    break;
                case unary_Inc:
                    if (!sema_type_is_errorish(inner) && !sema_type_is_numeric(inner)) {
                        char inner_name[128];
                        sema_error(s, expr->span, "operator '++' expects numeric operand, found %s",
                            sema_type_display_name(s, inner, inner_name, sizeof(inner_name)));
                    } else {
                        result = inner;
                    }
                    break;
                default:
                    result = inner;
                    break;
            }
            break;
        }
        case expr_Call: {
            // Snapshot the active handler stack before lowering this call, so
            // codegen (and the diagnostics layer) can later look up which
            // libmprompt evidence slot to use for any effect this call performs.
            sema_evidence_record_call(s, expr);
            struct Type* callee = sema_infer_expr(s, expr->call.callee);
            struct Decl* callee_decl = call_resolved_decl(expr);
            struct Type* spec = (callee_decl && sema_decl_is_generic(callee_decl))
                ? try_instantiate_call_site(s, expr)
                : NULL;
            if (spec) callee = spec;
            if (callee && callee->kind == TYPE_FUNCTION && callee->ret) {
                if (spec) {
                    check_generic_call_args(s, expr, spec, callee_decl);
                } else {
                    check_call_args(s, expr, callee);
                }
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
        case expr_Bind: {
            struct Decl* decl = expr->bind.name.resolved;
            struct SemaDeclInfo* info = decl ? sema_decl_info(s, decl) : NULL;
            bool decl_is_value = decl && decl->semantic_kind == SEM_VALUE;
            bool decl_is_function = decl_is_value && info && info->type &&
                info->type->kind == TYPE_FUNCTION &&
                expr_is_function_like(expr->bind.value);
            bool track_value_body = decl_is_value && !decl_is_function;

            if (track_value_body && info && info->type_query.state != QUERY_ERROR) {
                info->type_query.state = QUERY_RUNNING;
            }

            // Function-like decls get their own CheckedBody so facts produced
            // while inferring the body do not leak into the module body. Phase 5
            // will key generic instantiations off this same body shape.
            struct CheckedBody* fn_body = NULL;
            struct CheckedBody* fn_prev = NULL;
            if (decl_is_function) {
                struct Module* mod = s->current_body ? s->current_body->module : NULL;
                fn_body = sema_body_new(s, decl, mod, NULL);
                fn_prev = sema_enter_body(s, fn_body);
            }

            if (expr->bind.type_ann) {
                struct Type* expected = sema_infer_type_expr(s, expr->bind.type_ann);
                if (expr->bind.value) sema_check_expr(s, expr->bind.value, expected);
                result = expected;
            } else {
                result = sema_infer_expr(s, expr->bind.value);
            }

            if (decl_is_function) {
                sema_leave_body(s, fn_prev);
            }

            if (info && info->type_query.state != QUERY_ERROR && !sema_type_is_errorish(result)) {
                if (decl_is_function) {
                    merge_function_body_type(info->type, result);
                    result = info->type;
                    info->type_query.state = QUERY_DONE;
                    info->effect_sig = info->type ? info->type->effect_sig : NULL;
                } else if (decl_is_value) {
                    info->type = result;
                    info->type_query.state = QUERY_DONE;
                    info->effect_sig = result ? result->effect_sig : NULL;
                } else if (info->type) {
                    result = info->type;
                }
            } else if (track_value_body && info && info->type_query.state != QUERY_ERROR) {
                info->type_query.state = QUERY_DONE;
            }
            semantic = decl && decl->semantic_kind != SEM_UNKNOWN
                ? decl->semantic_kind
                : sema_semantic_for_type(result);
            break;
        }
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
        case expr_With: {
            // The handler-impl block lives in `with.func`; the user code that
            // sees the handler in scope lives in `with.body`. Push an evidence
            // frame for the duration of the body walk.
            sema_infer_expr(s, expr->with.func);

            // Resolver populated this on every name-resolved AST.
            struct Decl* eff_decl = expr->with.handled_effect;
            bool pushed = false;
            if (eff_decl) {
                struct EvidenceFrame frame = {
                    .effect_decl = eff_decl,
                    .handler_decl = call_resolved_decl(expr->with.func),
                    .scope_token_id = 0,
                    .with_expr = expr,
                };
                // For Ident/Field with.func, take handler_decl directly.
                if (!frame.handler_decl) {
                    if (expr->with.func && expr->with.func->kind == expr_Ident) {
                        frame.handler_decl = expr->with.func->ident.resolved;
                    } else if (expr->with.func && expr->with.func->kind == expr_Field) {
                        frame.handler_decl = expr->with.func->field.field.resolved;
                    }
                }
                sema_evidence_push(s->current_evidence, frame);
                pushed = true;
            }

            result = sema_infer_expr(s, expr->with.body);

            if (pushed) sema_evidence_pop(s->current_evidence);
            semantic = SEM_VALUE;
            break;
        }
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

            // Comptime substitution: if a comptime type-param is bound in the
            // active env, evaluate to the concrete type.
            if (s->current_env) {
                struct ConstValue v;
                if (sema_comptime_env_lookup(s->current_env, decl, &v) &&
                    v.kind == CONST_TYPE && v.type_val) {
                    return v.type_val;
                }
            }

            if (decl->semantic_kind == SEM_TYPE) return sema_type_of_decl(s, decl);

            struct Type* value_type = sema_type_of_decl(s, decl);
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
            if (decl && decl->semantic_kind == SEM_TYPE) return sema_type_of_decl(s, decl);
            struct Type* value_type = sema_type_of_decl(s, decl);
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
            struct Module* mod = mod_p ? *mod_p : NULL;
            if (!mod || !mod->ast) continue;
            struct CheckedBody* body = sema_body_new(s, NULL, mod, NULL);
            struct CheckedBody* prev = sema_enter_body(s, body);
            for (size_t j = 0; j < mod->ast->count; j++) {
                struct Expr** expr = (struct Expr**)vec_get(mod->ast, j);
                if (expr && *expr) sema_infer_expr(s, *expr);
            }
            sema_leave_body(s, prev);
        }
    } else if (s->resolver->ast) {
        struct CheckedBody* body = sema_body_new(s, NULL, NULL, NULL);
        struct CheckedBody* prev = sema_enter_body(s, body);
        for (size_t i = 0; i < s->resolver->ast->count; i++) {
            struct Expr** expr = (struct Expr**)vec_get(s->resolver->ast, i);
            if (expr && *expr) sema_infer_expr(s, *expr);
        }
        sema_leave_body(s, prev);
    }

    return !s->has_errors;
}
