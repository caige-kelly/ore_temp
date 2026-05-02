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

// True when the decl's bind value is marked as comptime — only meaningful
// for `::` bindings and other AST nodes the parser/resolver flagged as
// comptime. Used by call-site folding (sema_call_arg / decl values).
static bool sema_decl_is_comptime(struct Decl* decl) {
    if (!decl || !decl->node || decl->node->kind != expr_Bind) return false;
    struct Expr* value = decl->node->bind.value;
    return value && value->is_comptime;
}

static void try_set_array_length(struct Sema* s, struct Type* arr, struct Expr* size_expr) {
    if (!arr || !size_expr) return;
    struct EvalResult sr = sema_const_eval_expr(s, size_expr, NULL);
    if (sr.control == EVAL_NORMAL && sr.value.kind == CONST_INT && sr.value.int_val >= 0) {
        arr->array_length = sr.value.int_val;
    } else if (sr.control == EVAL_NORMAL && sr.value.kind != CONST_INVALID) {
        sema_error(s, size_expr->span, "array size must be a non-negative integer");
    }
}

// Build the type for an `expr_ArrayType` node.
// `[N]T` → array-of-T with N folded as length.
// `[^]T` → many-pointer-to-T (the parser sets `is_many_ptr` on the same node).
// Walks `array_type.size` for diagnostics whether or not we use the value.
// Used by both `sema_infer_expr` (value-position) and `sema_infer_type_expr`
// (type-position) — formerly two near-duplicate copies.
static struct Type* infer_array_type(struct Sema* s, struct Expr* expr) {
    sema_infer_expr(s, expr->array_type.size);
    if (expr->array_type.is_many_ptr) {
        return sema_pointer_type(s, sema_infer_type_expr(s, expr->array_type.elem));
    }
    struct Type* elem = sema_infer_type_expr(s, expr->array_type.elem);
    struct Type* arr  = sema_array_type(s, elem);
    try_set_array_length(s, arr, expr->array_type.size);
    return arr;
}

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
        // `sema_type_assignable(void, anything) == true` (see type.c),
        // so the void case naturally falls out of normal checking.
        sema_check_expr(s, body, fn->ret);
    }
    return fn;
}

static bool expr_is_function_like(struct Expr* expr) {
    return expr && (expr->kind == expr_Lambda || expr->kind == expr_Ctl);
}

static void check_value_fits(struct Sema* s, struct Expr* src, struct Type* target) {
    if (!src || !target) return;
    if (!sema_type_is_integer(target) && !sema_type_is_float(target)) return;

    struct EvalResult vr = sema_const_eval_expr(s, src, NULL);
    if (vr.control != EVAL_NORMAL || vr.value.kind == CONST_INVALID) return;

    if (!sema_value_fits_type(vr.value, target)) {
        char target_name[128];
        sema_error(s, src->span,
            "value %lld does not fit in %s",
            (long long)vr.value.int_val,
            sema_type_display_name(s, target, target_name, sizeof(target_name)));
    }
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
    if (!expr || expr->kind != expr_Product || !expected) return false;
    // Allow `?Header.{...}` and `?Header` from context — strip one optional layer.
    if (expected->is_optional) expected = sema_unwrap_optional(s, expected);
    if (!expected || expected->kind != TYPE_STRUCT) return false;
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
                    sema_type_display_name(s, expected, type_name, sizeof(type_name)),
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
                        sema_type_display_name(s, expected, type_name, sizeof(type_name)));
                    ok = false;
                }
            }
        }
    }

    sema_record_fact(s, expr, expected, SEM_VALUE, expected->region_id);
    return ok;
}

// Bind an `if (opt) |x|` or `loop (opt) |x|` capture decl's type from the
// unwrapped optional condition, and diagnose if the condition isn't
// optional. `construct_label` ("if-let" / "loop") personalizes the
// diagnostic. Caller is responsible for the no-capture branch — this
// helper only handles the capture-present case.
static void bind_optional_capture(struct Sema* s, struct Identifier capture,
    struct Type* cond_t, struct Span cond_span, const char* construct_label) {
    if (capture.string_id == 0 || !cond_t) return;
    struct Decl* cap_decl = capture.resolved;
    struct Type* unwrapped = cond_t->is_optional
        ? sema_unwrap_optional(s, cond_t) : cond_t;
    if (cap_decl) {
        struct SemaDeclInfo* info = sema_decl_info(s, cap_decl);
        if (info) {
            info->type = unwrapped;
            info->type_query.state = QUERY_DONE;
        }
    }
    if (!cond_t->is_optional && !sema_type_is_errorish(cond_t)) {
        char name[128];
        sema_error(s, cond_span,
            "%s capture |x| requires an optional condition, found %s",
            construct_label,
            sema_type_display_name(s, cond_t, name, sizeof(name)));
    }
}

// Non-tail statements in a block must produce no observable value. Anything
// that types to a real value (int, struct, etc.) and isn't already a
// statement-shaped expr (Bind/Assign/Loop type as void; Return/Break/Continue
// as noreturn; errored exprs as error) is almost certainly an
// accidental discard. Force the user to opt in via `_ = expr`.
static void diagnose_discarded_stmt(struct Sema* s, struct Expr* stmt,
                                    struct Type* t) {
    if (!stmt || !t) return;
    // Statement-shaped exprs that happen to type as their RHS value (Bind
    // and DestructureBind both record the inferred type instead of void
    // because the type query reads it back). They aren't actually
    // discarding anything — the binding is the side effect.
    if (stmt->kind == expr_Bind || stmt->kind == expr_DestructureBind) return;
    if (sema_type_is_errorish(t)) return;
    switch (t->kind) {
        case TYPE_VOID:
        case TYPE_NORETURN:
        case TYPE_UNKNOWN:
        case TYPE_ANYTYPE:
            return;
        default:
            break;
    }
    char nm[128];
    sema_error(s, stmt->span,
        "unused result of type %s (use `_ = expr` to discard explicitly)",
        sema_type_display_name(s, t, nm, sizeof(nm)));
}

static bool check_block_expr(struct Sema* s, struct Expr* expr, struct Type* expected) {
    if (!expr || expr->kind != expr_Block) return false;
    Vec* stmts = expr->block.stmts;
    if (stmts->count == 0) {
        struct Type* actual = s->void_type;
        if (!sema_type_assignable(expected, actual)) {
            report_type_mismatch(s, expr->span, expected, actual);
            // Record the fact even on failure so downstream tools (HIR
            // lowering, dump_tyck) see every expr in the body, not just the
            // ones that type-checked successfully.
            sema_record_fact(s, expr, s->error_type, SEM_VALUE, 0);
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
        struct Type* t = sema_infer_expr(s, *stmt);
        diagnose_discarded_stmt(s, *stmt, t);
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
            sema_type_display_name(s, expected, expected_name, sizeof(expected_name)));
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

// Detect a desugared `with` Call: a Call whose only arg is a Lambda.
// Returns the Lambda when the shape matches, NULL otherwise.
static struct Expr* with_shape_action_lambda(struct Expr* call_expr) {
    if (!call_expr || call_expr->kind != expr_Call) return NULL;
    if (!call_expr->call.args || call_expr->call.args->count != 1) return NULL;
    struct Expr** a0p = (struct Expr**)vec_get(call_expr->call.args, 0);
    struct Expr* a0 = a0p ? *a0p : NULL;
    if (!a0 || a0->kind != expr_Lambda) return NULL;
    return a0;
}

// Resolve the handled-effect decl for a desugared-with Call. Two paths,
// matching the resolver's overlay logic:
//   (a) callee is a function decl whose lambda-typed param has an effect
//       annotation referencing some effect E → return E.
//   (b) callee is a Block whose first bind names an op of some effect
//       in scope → return that effect (legacy handler-block fallback).
// Phase 0 transitional helper; Phase 3 will type the callee directly.
static struct Decl* with_shape_handled_effect(struct Sema* s,
                                              struct Expr* call_expr) {
    if (!s || !with_shape_action_lambda(call_expr)) return NULL;

    struct Decl* d = call_resolved_decl(call_expr);

    // (a) Walk callee's lambda-param annotations for an effect reference.
    if (d && d->node && d->node->kind == expr_Bind &&
        d->node->bind.value &&
        d->node->bind.value->kind == expr_Lambda) {
        Vec* params = d->node->bind.value->lambda.params;
        struct Expr* eff = NULL;
        if (params) {
            for (size_t pi = 0; pi < params->count && !eff; pi++) {
                struct Param* p = (struct Param*)vec_get(params, pi);
                if (p && p->type_ann && p->type_ann->kind == expr_Lambda) {
                    eff = p->type_ann->lambda.effect;
                }
            }
        }
        if (eff) {
            // Tiny iterative walk; no arena needed for the depths we see.
            struct Expr* stack[64];
            size_t n = 0;
            stack[n++] = eff;
            while (n > 0) {
                struct Expr* e2 = stack[--n];
                if (!e2) continue;
                struct Decl* ed = ast_resolved_decl_of(e2);
                if (ed && ed->semantic_kind == SEM_EFFECT) return ed;
                switch (e2->kind) {
                    case expr_Bin:
                        if (n + 2 < 64) {
                            if (e2->bin.Left)  stack[n++] = e2->bin.Left;
                            if (e2->bin.Right) stack[n++] = e2->bin.Right;
                        }
                        break;
                    case expr_Call:
                        if (n < 64 && e2->call.callee) stack[n++] = e2->call.callee;
                        break;
                    case expr_Field:
                        if (n < 64 && e2->field.object) stack[n++] = e2->field.object;
                        break;
                    case expr_EffectRow:
                        if (n < 64 && e2->effect_row.head) stack[n++] = e2->effect_row.head;
                        break;
                    default:
                        break;
                }
            }
        }
    }

    // (b) Handler-literal callee: the resolver already ran set-equality
    // matching when it visited the expr_Handler node and stashed the
    // result on `handler.effect_decl`. Just read it.
    struct Expr* callee = call_expr->call.callee;
    if (callee && callee->kind == expr_Handler) {
        return callee->handler.effect_decl;
    }

    return NULL;
}

// Per-op signature check for `expr_Handler`. Looks up the matching effect
// op decl by name, verifies arity and fn-vs-ctl agreement, type-checks
// each typed param against the effect-declared param type, and
// **backfills** untyped handler op params from the effect signature so
// the body can type-check against meaningful param types.
//
// Phase 2's set-equality matcher guarantees `effect_decl` is non-NULL
// only when every op name in the handler matches one in the effect; if
// that fails we never reach this helper. The defensive lookup here
// handles malformed AST without crashing.
static void check_handler_op_against_effect(struct Sema* s,
                                             struct HandlerOp* op,
                                             struct Decl* effect_decl) {
    if (!s || !op || !effect_decl || !effect_decl->child_scope) return;

    struct Decl* eff_op = (struct Decl*)hashmap_get(
        &effect_decl->child_scope->name_index, (uint64_t)op->name.string_id);
    if (!eff_op) return;

    struct Type* eff_type = sema_type_of_decl(s, eff_op);
    if (!eff_type || eff_type->kind != TYPE_FUNCTION) {
        // Effect op decl didn't resolve to a function — likely an
        // unrelated diagnostic already fired. Walk the body for
        // resolution and bail.
        if (op->body) sema_infer_expr(s, op->body);
        return;
    }

    // Effect op fn-vs-ctl-ness comes from its bind value's AST kind.
    bool eff_is_ctl = false;
    if (eff_op->node && eff_op->node->kind == expr_Bind &&
        eff_op->node->bind.value &&
        eff_op->node->bind.value->kind == expr_Ctl) {
        eff_is_ctl = true;
    }
    if (op->is_ctl != eff_is_ctl) {
        const char* nm = pool_get(s->pool, op->name.string_id, 0);
        sema_error(s, op->span,
            "handler op '%s' is declared as %s but effect declares %s",
            nm ? nm : "?",
            op->is_ctl ? "ctl" : "fn",
            eff_is_ctl ? "ctl" : "fn");
    }

    Vec* eff_params = eff_type->params;
    size_t eff_arity = eff_params ? eff_params->count : 0;
    size_t op_arity = op->params ? op->params->count : 0;
    if (eff_arity != op_arity) {
        const char* nm = pool_get(s->pool, op->name.string_id, 0);
        sema_error(s, op->span,
            "handler op '%s' takes %zu params but effect declares %zu",
            nm ? nm : "?", op_arity, eff_arity);
    }

    size_t check_count = eff_arity < op_arity ? eff_arity : op_arity;
    for (size_t i = 0; i < check_count; i++) {
        struct Param* p = (struct Param*)vec_get(op->params, i);
        struct Type** ept = (struct Type**)vec_get(eff_params, i);
        struct Type* expected = ept ? *ept : NULL;
        if (!p || !expected) continue;

        if (p->type_ann) {
            // User-supplied annotation must match the effect's param type.
            struct Type* actual = sema_infer_type_expr(s, p->type_ann);
            if (actual && expected && !sema_type_equal(actual, expected) &&
                !sema_type_is_errorish(actual) &&
                !sema_type_is_errorish(expected)) {
                char eb[128], ab[128];
                const char* pname = pool_get(s->pool, p->name.string_id, 0);
                sema_error(s, p->name.span,
                    "handler op param '%s' has type %s but effect declares %s",
                    pname ? pname : "?",
                    sema_type_display_name(s, actual, ab, sizeof(ab)),
                    sema_type_display_name(s, expected, eb, sizeof(eb)));
            }
        } else {
            // Backfill: store the effect-declared param type on the
            // handler's param decl so body lookups see a real type.
            struct Decl* p_decl = p->name.resolved;
            if (p_decl) {
                struct SemaDeclInfo* info = sema_decl_info(s, p_decl);
                if (info && !info->type) info->type = expected;
            }
        }
    }

    // Type-check the body against the effect op's declared return type.
    // void's "discard anything" rule lives in sema_type_assignable, so
    // there's no special case here — void-returning ops fall out
    // naturally.
    if (op->body) {
        if (eff_type->ret && !sema_type_is_errorish(eff_type->ret)) {
            sema_check_expr(s, op->body, eff_type->ret);
        } else {
            sema_infer_expr(s, op->body);
        }
    }
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

    // Visible arity excludes INFERRED params — those are filled by sema from
    // context (e.g. scope tokens from the active evidence vector) and never
    // appear at the call site.
    size_t expected = sema_param_visible_arity(generic_params);
    size_t actual = args->count;
    if (expected != actual) {
        sema_error(s, call_expr->span, "expected %zu arguments but found %zu",
            expected, actual);
    }

    // Walk param positions in declaration order; advance a separate cursor
    // through the call's arg list, skipping over INFERRED params (no arg
    // supplied at the call site).
    size_t arg_i = 0;
    size_t runtime_i = 0;
    for (size_t i = 0; i < generic_params->count && arg_i < args->count; i++) {
        struct Param* p = (struct Param*)vec_get(generic_params, i);
        if (!p) continue;
        if (p->kind == PARAM_INFERRED_COMPTIME) continue;

        struct Expr** arg_p = (struct Expr**)vec_get(args, arg_i);
        struct Expr* arg = arg_p ? *arg_p : NULL;
        arg_i++;
        if (!arg) continue;

        if (p->kind == PARAM_COMPTIME) {
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

    for (size_t i = arg_i; i < args->count; i++) {
        struct Expr** arg_p = (struct Expr**)vec_get(args, i);
        if (arg_p && *arg_p) sema_infer_expr(s, *arg_p);
    }
}

// Look up a scope token for an INFERRED_COMPTIME `s: Scope` param by walking
// the active evidence vector. Today the rule is: take the innermost frame
// whose handler-decl exists; its scope_token_id is what we bind. Future:
// match the param to a specific effect via the callee's annotation.
static struct ConstValue infer_scope_param(struct Sema* s) {
    if (!s || !s->current_evidence || !s->current_evidence->frames) {
        return sema_const_invalid();
    }
    Vec* frames = s->current_evidence->frames;
    for (size_t i = frames->count; i > 0; i--) {
        struct EvidenceFrame* f = (struct EvidenceFrame*)vec_get(frames, i - 1);
        if (!f) continue;
        if (f->scope_token_id != 0) return sema_const_int((int64_t)f->scope_token_id);
        // Fall back to the effect's own scope_token_id (some effects carry it
        // directly on their Decl when declared `scoped effect<s>`).
        if (f->effect_decl && f->effect_decl->scope_token_id != 0) {
            return sema_const_int((int64_t)f->effect_decl->scope_token_id);
        }
    }
    return sema_const_invalid();
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

    struct ComptimeArgTuple tuple = sema_arg_tuple_new(s);
    bool ok = true;
    size_t arg_i = 0;

    for (size_t i = 0; i < params->count; i++) {
        struct Param* p = (struct Param*)vec_get(params, i);
        if (!p) continue;

        if (p->kind == PARAM_INFERRED_COMPTIME) {
            // Sema fills this param from context. Today only Scope-typed
            // inferred params exist; pull the token from the evidence stack.
            struct ConstValue v = infer_scope_param(s);
            if (!sema_const_value_is_valid(v)) {
                const char* pname = s->pool ? pool_get(s->pool, p->name.string_id, 0) : "?";
                sema_error(s, call_expr->span,
                    "no enclosing handler provides scope for inferred parameter '%s'",
                    pname ? pname : "?");
                ok = false;
                continue;
            }
            sema_arg_tuple_push(&tuple, v);
            continue;
        }

        if (p->kind == PARAM_RUNTIME) {
            arg_i++;
            continue;
        }

        // PARAM_COMPTIME — consume the next caller-supplied arg.
        struct Expr** arg_p = args ? (struct Expr**)vec_get(args, arg_i) : NULL;
        struct Expr* arg = arg_p ? *arg_p : NULL;
        arg_i++;
        if (!arg) { ok = false; continue; }

        struct EvalResult v = sema_const_eval_expr(s, arg, s->current_env);
        if (!sema_const_value_is_valid(v.value)) {
            const char* pname = s->pool ? pool_get(s->pool, p->name.string_id, 0) : "?";
            sema_error(s, arg->span,
                "comptime argument '%s' must be known at compile time",
                pname ? pname : "?");
            ok = false;
            continue;
        }
        sema_arg_tuple_push(&tuple, v.value);
    }

    if (!ok) return NULL;

    struct Instantiation* inst = sema_instantiate_decl(s, callee, tuple);
    return inst ? inst->specialized_type : NULL;
}

struct Type* sema_infer_expr(struct Sema* s, struct Expr* expr) {
    if (!expr) return s->void_type;
    struct ConstValue comptime_value = sema_const_invalid();

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
            struct Type* obj_type = sema_infer_expr(s, expr->field.object);
            struct Decl* decl = expr->field.field.resolved;

            // If name-res didn't pre-link the field (typical for value-level
            // accesses like `header^.size` where the object's type is only
            // known at sema time), find the field via the object's type.
            // Auto-deref a single pointer level so `p.f` works the same as
            // `p^.f` for the common case. Modules and structs both expose
            // their members via `child_scope`.
            bool looked_up_dynamically = false;
            if (!decl && obj_type) {
                struct Type* probe = obj_type;
                if (probe && probe->kind == TYPE_POINTER && probe->elem) {
                    probe = probe->elem;
                }
                if (probe && probe->decl && probe->decl->child_scope &&
                    (probe->kind == TYPE_STRUCT ||
                     probe->kind == TYPE_MODULE ||
                     probe->kind == TYPE_ENUM ||
                     probe->kind == TYPE_EFFECT)) {
                    decl = (struct Decl*)hashmap_get(
                        &probe->decl->child_scope->name_index,
                        (uint64_t)expr->field.field.string_id);
                    looked_up_dynamically = true;
                    if (decl) expr->field.field.resolved = decl;
                }
            }

            result = sema_type_of_decl(s, decl);
            // Only diagnose "no field" when we actually attempted a lookup
            // against a known type-bearing object. Don't fire for the
            // common pre-resolution case where name-res hasn't run for
            // some import paths yet.
            if (looked_up_dynamically && !decl && obj_type &&
                !sema_type_is_errorish(obj_type)) {
                const char* fname = s->pool
                    ? pool_get(s->pool, expr->field.field.string_id, 0) : "?";
                char tname[128];
                sema_error(s, expr->span,
                    "no field '%s' on type %s",
                    fname ? fname : "?",
                    sema_type_display_name(s, obj_type, tname, sizeof(tname)));
            }
            semantic = decl ? decl->semantic_kind : sema_semantic_for_type(result);
            region_id = result ? result->region_id : 0;
            break;
        }
        case expr_Builtin: {
            uint32_t bn = expr->builtin.name_id;
            if (bn == s->name_import) {
                result = s->module_type;
                semantic = SEM_MODULE;
            } else if (bn == s->name_sizeOf || bn == s->name_alignOf) {
                result = s->comptime_int_type;
                semantic = SEM_VALUE;
                bool is_size = (bn == s->name_sizeOf);
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
            } else if (bn == s->name_intCast) {
                result = s->comptime_int_type;
                semantic = SEM_VALUE;
                if (expr->builtin.args) {
                    for (size_t i = 0; i < expr->builtin.args->count; i++) {
                        struct Expr** arg = (struct Expr**)vec_get(expr->builtin.args, i);
                        if (arg) sema_infer_expr(s, *arg);
                    }
                }
            } else if (bn == s->name_TypeOf) {
                result = s->type_type;
                semantic = SEM_TYPE;
                if (expr->builtin.args) {
                    for (size_t i = 0; i < expr->builtin.args->count; i++) {
                        struct Expr** arg = (struct Expr**)vec_get(expr->builtin.args, i);
                        if (arg) sema_infer_expr(s, *arg);
                    }
                }
            } else if (bn == s->name_returnType) {
                // `@returnType(action)` produces the return type of a
                // function-typed value. Used in handler signatures and
                // anywhere a wrapper wants to declare "I return what my
                // callee returns". The actual extraction happens in
                // const_eval; here we just type the expression as a Type.
                result = s->type_type;
                semantic = SEM_TYPE;
                if (expr->builtin.args) {
                    for (size_t i = 0; i < expr->builtin.args->count; i++) {
                        struct Expr** arg = (struct Expr**)vec_get(expr->builtin.args, i);
                        if (arg) sema_infer_expr(s, *arg);
                    }
                }
            } else {
                // Unknown @-builtin in expression position. Diagnose
                // explicitly — silent `unknown_type` makes typo'd or
                // missing builtins ("@notabuiltin") propagate as a
                // confusing downstream error rather than an actionable
                // one. const_eval errors on unknown builtins too; this
                // covers the type-check side.
                const char* name = s->pool
                    ? pool_get(s->pool, expr->builtin.name_id, 0) : NULL;
                sema_error(s, expr->span,
                    "unknown comptime builtin '@%s'",
                    name ? name : "?");
                result = s->error_type;
                semantic = SEM_VALUE;
                if (expr->builtin.args) {
                    for (size_t i = 0; i < expr->builtin.args->count; i++) {
                        struct Expr** arg = (struct Expr**)vec_get(expr->builtin.args, i);
                        if (arg) sema_infer_expr(s, *arg);
                    }
                }
            }
            break;
        }
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
                        sema_error(s, expr->span, "operator '%s' expects integer operands, found %s and %s",
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

                // `opt orelse fallback` — if `opt: ?T`, the fallback is the
                // value used when opt is nil. Result is `T`. Right side may
                // also be `noreturn` (e.g. `... orelse panic("…")`), in which
                // case the result is still `T` because noreturn is bottom.
                case OrElse: {
                    if (sema_type_is_errorish(left)) {
                        result = right ? right : s->unknown_type;
                        break;
                    }
                    if (!left->is_optional) {
                        char left_name[128];
                        sema_error(s, expr->span,
                            "`orelse` left side must be optional, found %s",
                            sema_type_display_name(s, left, left_name, sizeof(left_name)));
                        result = left;
                        break;
                    }
                    struct Type* unwrapped = sema_unwrap_optional(s, left);
                    if (right && right->kind == TYPE_NORETURN) {
                        result = unwrapped;
                        break;
                    }
                    if (right && !sema_type_assignable(unwrapped, right)) {
                        char want[128], got[128];
                        sema_error(s, expr->span,
                            "`orelse` right side type %s does not match unwrapped left %s",
                            sema_type_display_name(s, right, got, sizeof(got)),
                            sema_type_display_name(s, unwrapped, want, sizeof(want)));
                    }
                    result = unwrapped;
                    break;
                }

                default:
                    result = s->unknown_type;
                    break;
            }
            break;
        }

        case expr_Assign: {
            // L-value gate: target must be something we can write to. Today:
            // an Ident bound to a `:=` (mutable) decl, a Field access, an
            // Index access, or a deref `p^`. Other shapes are non-l-values.
            struct Expr* tgt = expr->assign.target;
            bool is_lvalue = false;
            if (tgt) {
                if (tgt->kind == expr_Ident) {
                    struct Decl* d = tgt->ident.resolved;
                    is_lvalue = d != NULL;
                    if (d && d->node && d->node->kind == expr_Bind &&
                        d->node->bind.kind == bind_Const) {
                        const char* nm = s->pool ? pool_get(s->pool, d->name.string_id, 0) : "?";
                        sema_error(s, expr->span,
                            "cannot assign to constant binding '%s' (declared with `::`)",
                            nm ? nm : "?");
                        // Suppress the generic "not assignable" cascade — we
                        // already reported the specific reason.
                        is_lvalue = true;
                    }
                } else if (tgt->kind == expr_Field) {
                    is_lvalue = true;
                } else if (tgt->kind == expr_Index) {
                    is_lvalue = true;
                } else if (tgt->kind == expr_Unary && tgt->unary.op == unary_Deref) {
                    is_lvalue = true;
                } else if (tgt->kind == expr_Wildcard) {
                    // `_ = expr` — explicit discard. We still type-check the
                    // RHS so it can't hide diagnostics, but accept any type.
                    is_lvalue = true;
                }
            }
            struct Type* tgt_t = sema_infer_expr(s, tgt);
            struct Type* val_t = sema_infer_expr(s, expr->assign.value);

            if (!is_lvalue && tgt && !sema_type_is_errorish(tgt_t)) {
                sema_error(s, expr->span,
                    "left side of `=` is not an assignable target");
            }

            if (tgt_t && val_t && !sema_type_is_errorish(tgt_t) &&
                !sema_type_assignable(tgt_t, val_t)) {
                char want[128], got[128];
                sema_error(s, expr->span,
                    "cannot assign %s to %s",
                    sema_type_display_name(s, val_t, got, sizeof(got)),
                    sema_type_display_name(s, tgt_t, want, sizeof(want)));
            }
            result = s->void_type;
            semantic = SEM_VALUE;
            break;
        }

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
                    } else {
                        result = inner;            // <-- preserve operand type
                    }
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

            // Phase 0 desugar: `with f body` ≡ `f(fn() body)`. When this
            // Call matches the shape AND we can identify a handled effect,
            // treat it as the old `expr_With` did — push an evidence frame,
            // walk the body lambda, return the body's type. Skip normal
            // call-type-checking because:
            //   - the body lambda doesn't supply f's other params (e.g.
            //     debug_allocator's inferred Scope) directly,
            //   - and the legacy `handler { ... }` Block callee isn't
            //     even a function value yet.
            // Phase 3 will type handlers as `HandlerOf<E>` and let normal
            // call typing handle this; until then, this is the bridge.
            struct Decl* with_eff = with_shape_handled_effect(s, expr);
            if (with_eff) {
                bool pushed = false;
                if (s->current_evidence) {
                    struct EvidenceFrame frame = {
                        .effect_decl = with_eff,
                        .handler_decl = call_resolved_decl(expr),
                        .scope_token_id = 0,
                    };
                    sema_evidence_push(s->current_evidence, frame);
                    pushed = true;
                }
                // Walk the callee for its identifier resolutions (so
                // diagnostics and dump-ast still show resolved decls), but
                // discard the type — we're not actually invoking it as a
                // function in the type system.
                if (expr->call.callee) sema_infer_expr(s, expr->call.callee);
                struct Expr* lam = with_shape_action_lambda(expr);
                struct Type* lam_type = lam ? sema_infer_expr(s, lam) : NULL;
                if (pushed) sema_evidence_pop(s->current_evidence);
                result = (lam_type && lam_type->kind == TYPE_FUNCTION && lam_type->ret)
                    ? lam_type->ret : (lam_type ? lam_type : s->void_type);
                semantic = SEM_VALUE;
                break;
            }

            struct Type* callee = sema_infer_expr(s, expr->call.callee);
            struct Decl* callee_decl = call_resolved_decl(expr);
            struct Type* spec = (callee_decl && sema_decl_is_generic(callee_decl))
                ? try_instantiate_call_site(s, expr)
                : NULL;
            if (spec) callee = spec;

            if (callee_decl && sema_decl_is_comptime(callee_decl)) {
                struct EvalResult er = sema_const_eval_expr(s, expr, NULL);
                if (er.control == EVAL_NORMAL && er.value.kind != CONST_INVALID) {
                    // NEW: range-check the folded return value against the function's
                    // declared return type.
                    if (callee->ret && !sema_value_fits_type(er.value, callee->ret)) {
                        char target[128];
                        sema_error(s, expr->span,
                            "comptime call returned value %lld which does not fit in %s",
                            (long long)er.value.int_val,
                            sema_type_display_name(s, callee->ret, target,
                                sizeof(target)));
                    }
                    comptime_value = er.value;
                }
            }

            if (callee && callee->kind == TYPE_FUNCTION && callee->ret) {
                if (spec) check_generic_call_args(s, expr, spec, callee_decl);
                else      check_call_args(s, expr, callee);
                result = callee->ret;
                // The comptime fold (and its return-value range check) ran
                // above, before arg checking. comptime_value is already set
                // if the call folded; no second eval needed here.
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
                        sema_type_display_name(s, callee, callee_name, sizeof(callee_name)));
                }
                result = s->unknown_type;
            }
            // Reached only by FUNCTION and "non-callable" branches above —
            // the TYPE_EFFECT branch sets SEM_EFFECT and breaks out of the
            // outer switch directly, so this assignment doesn't clobber it.
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
        case expr_Handler: {
            // Phase 3: type the handler node, run per-op signature checks
            // against the matched effect's op decls, and emit
            // `HandlerOf<E, R>` (or fall back to unknown_type when E is
            // NULL because Phase 2's set-equality matcher rejected).
            if (expr->handler.target) sema_infer_expr(s, expr->handler.target);
            struct Decl* eff = expr->handler.effect_decl;
            if (expr->handler.operations) {
                for (size_t i = 0; i < expr->handler.operations->count; i++) {
                    struct HandlerOp** opp = (struct HandlerOp**)vec_get(expr->handler.operations, i);
                    struct HandlerOp* op = opp ? *opp : NULL;
                    if (!op) continue;
                    if (eff) {
                        check_handler_op_against_effect(s, op, eff);
                    } else if (op->body) {
                        // Phase 2 already diagnosed the no-match; still
                        // walk the body so name-resolution facts land.
                        sema_infer_expr(s, op->body);
                    }
                }
            }
            sema_infer_expr(s, expr->handler.initially_clause);
            sema_infer_expr(s, expr->handler.finally_clause);
            struct Type* return_ty = expr->handler.return_clause
                ? sema_infer_expr(s, expr->handler.return_clause)
                : NULL;

            if (expr->handler.target) {
                // `handle (target) { ops }` — value is the handled
                // action's return type. R comes from `return_clause`
                // when present; otherwise fall back to whatever the
                // target's type yields.
                result = return_ty ? return_ty : s->unknown_type;
            } else if (eff) {
                // `handler { ops }` — emits a HandlerOf<E, R> value.
                // R is NULL when there's no `return` clause; equality
                // treats NULL as a wildcard for now (Phase 3 scope).
                result = sema_handler_type(s, eff, return_ty);
            } else {
                result = s->unknown_type;
            }
            semantic = SEM_VALUE;
            break;
        }
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
                if (expr->bind.value) {
                    sema_check_expr(s, expr->bind.value, expected);
                    check_value_fits(s, expr->bind.value, expected);
                }
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
            Vec* stmts = expr->block.stmts;
            for (size_t i = 0; i < stmts->count; i++) {
                struct Expr** stmt = (struct Expr**)vec_get(stmts, i);
                if (!stmt || !*stmt) continue;
                struct Type* st = sema_infer_expr(s, *stmt);
                if (i + 1 == stmts->count) {
                    result = st;     // tail expression is the block's value
                } else {
                    diagnose_discarded_stmt(s, *stmt, st);
                }
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
        case expr_If: {
            struct Type* cond_t = sema_infer_expr(s, expr->if_expr.condition);

            // `if (opt) |x| then ... else ...` binds `x` as the unwrapped
            // value of an optional condition for the then-branch.
            if (expr->if_expr.capture.string_id != 0) {
                bind_optional_capture(s, expr->if_expr.capture, cond_t,
                    expr->if_expr.condition->span, "if-let");
            } else if (cond_t && !sema_type_is_errorish(cond_t) &&
                       cond_t->kind != TYPE_BOOL && !cond_t->is_optional) {
                char name[128];
                sema_error(s, expr->if_expr.condition->span,
                    "if condition must be bool or optional, found %s",
                    sema_type_display_name(s, cond_t, name, sizeof(name)));
            }

            if (expr->is_comptime) {
                struct EvalResult cr = sema_const_eval_expr(s, expr->if_expr.condition, NULL);
                if (cr.control == EVAL_NORMAL && cr.value.kind == CONST_BOOL) {
                    // Pick a branch. Only type-check the picked one; the dead branch
                    // is skipped entirely (its AST is never walked).
                    struct Expr* picked = cr.value.bool_val
                        ? expr->if_expr.then_branch
                        : expr->if_expr.else_branch;
                    if (picked) {
                        result = sema_infer_expr(s, picked);
                    } else {
                        result = s->void_type;   // false condition, no else branch
                    }
                    semantic = SEM_VALUE;
                    break;
                }
                // Condition didn't fold — error and fall through to the runtime path
                // so type-checking continues with both branches checked.
                sema_error(s, expr->if_expr.condition->span,
                    "`comptime if` condition must be a comptime-known boolean");
            }
        
            // Existing runtime path: walk both branches.
            result = sema_infer_expr(s, expr->if_expr.then_branch);
            sema_infer_expr(s, expr->if_expr.else_branch);
            semantic = SEM_VALUE;
            break;
        }
        case expr_Switch: {
            struct Type* scrut_t = sema_infer_expr(s, expr->switch_expr.scrutinee);

            if (expr->is_comptime) {
                struct EvalResult sr = sema_const_eval_expr(s, expr->switch_expr.scrutinee, NULL);
                if (sr.control != EVAL_NORMAL || sr.value.kind == CONST_INVALID) {
                    sema_error(s, expr->switch_expr.scrutinee->span,
                        "`comptime switch` scrutinee must be a comptime-known value");
                    // Fall through to runtime path so the rest of the file still type-checks.
                } else {
                    struct ConstValue scrut_val = sr.value;
                    struct SwitchArm* picked = NULL;
        
                    // Walk arms; first matching arm wins. The arm matches if any of its
                    // patterns folds to a value equal to the scrutinee.
                    if (expr->switch_expr.arms) {
                        for (size_t i = 0; i < expr->switch_expr.arms->count && !picked; i++) {
                            struct SwitchArm* arm = (struct SwitchArm*)vec_get(expr->switch_expr.arms, i);
                            if (!arm || !arm->patterns) continue;
        
                            for (size_t j = 0; j < arm->patterns->count; j++) {
                                struct Expr** pat_p = (struct Expr**)vec_get(arm->patterns, j);
                                struct Expr* pat = pat_p ? *pat_p : NULL;
                                if (!pat) continue;

                                // `_` matches anything — covers fall-through arms.
                                if (pat->kind == expr_Wildcard) {
                                    picked = arm;
                                    break;
                                }

                                struct EvalResult pr = sema_const_eval_expr(s, pat, NULL);
                                if (pr.control != EVAL_NORMAL) continue;

                                if (sema_const_value_equal(pr.value, scrut_val)) {
                                    picked = arm;
                                    break;
                                }
                            }
                        }
                    }
        
                    if (picked) {
                        result = sema_infer_expr(s, picked->body);
                        semantic = SEM_VALUE;
                        break;
                    }
        
                    // No arm matched. Error and fall through to the runtime path.
                    sema_error(s, expr->span,
                        "`comptime switch` has no matching arm");
                }
            }        

            // Each arm pattern should be assignable to the scrutinee type.
            // For `.Variant` shorthand patterns (expr_EnumRef) on an enum
            // scrutinee, route through check_expr so the variant gets
            // looked up and the pattern is typed with the enum's Type.
            // Each arm body's type joins with the others; the switch's
            // resulting type is that join.
            //
            // Exhaustiveness: when the scrutinee is an enum (and no wildcard
            // arm is present), every variant must appear in some pattern.
            bool scrut_is_enum = scrut_t && scrut_t->kind == TYPE_ENUM &&
                                 scrut_t->decl && scrut_t->decl->child_scope;
            bool saw_wildcard = false;
            Vec* covered_ids = NULL;
            if (scrut_is_enum) {
                covered_ids = vec_new_in(s->arena, sizeof(uint32_t));
            }
            struct Type* joined = NULL;
            if (expr->switch_expr.arms) {
                for (size_t i = 0; i < expr->switch_expr.arms->count; i++) {
                    struct SwitchArm* arm = (struct SwitchArm*)vec_get(expr->switch_expr.arms, i);
                    if (!arm) continue;
                    if (arm->patterns) {
                        for (size_t j = 0; j < arm->patterns->count; j++) {
                            struct Expr** pat_p = (struct Expr**)vec_get(arm->patterns, j);
                            struct Expr* pat = pat_p ? *pat_p : NULL;
                            if (!pat) continue;
                            if (pat->kind == expr_Wildcard) {
                                sema_infer_expr(s, pat);
                                saw_wildcard = true;
                                continue;
                            }
                            if (scrut_is_enum && pat->kind == expr_EnumRef) {
                                if (sema_check_expr(s, pat, scrut_t) && covered_ids) {
                                    uint32_t id = pat->enum_ref_expr.name.string_id;
                                    vec_push(covered_ids, &id);
                                }
                                continue;
                            }
                            struct Type* pat_t = sema_infer_expr(s, pat);
                            if (scrut_t && pat_t &&
                                !sema_type_is_errorish(scrut_t) &&
                                !sema_type_is_errorish(pat_t) &&
                                !sema_type_assignable(scrut_t, pat_t)) {
                                char st[128], pt[128];
                                sema_error(s, pat->span,
                                    "switch pattern type %s does not match scrutinee %s",
                                    sema_type_display_name(s, pat_t, pt, sizeof(pt)),
                                    sema_type_display_name(s, scrut_t, st, sizeof(st)));
                            }
                        }
                    }
                    struct Type* body_t = sema_infer_expr(s, arm->body);
                    if (!joined) {
                        joined = body_t;
                    } else if (body_t && body_t->kind == TYPE_NORETURN) {
                        // noreturn arms don't constrain the join.
                    } else if (joined->kind == TYPE_NORETURN) {
                        joined = body_t;
                    } else if (body_t && !sema_type_assignable(joined, body_t)) {
                        char a[128], b[128];
                        sema_error(s, arm->body ? arm->body->span : expr->span,
                            "switch arm type %s incompatible with prior arm %s",
                            sema_type_display_name(s, body_t, b, sizeof(b)),
                            sema_type_display_name(s, joined, a, sizeof(a)));
                    }
                }
            }

            // Exhaustiveness: complain about every variant left uncovered.
            if (scrut_is_enum && !saw_wildcard && covered_ids) {
                struct Scope* enum_scope = scrut_t->decl->child_scope;
                Vec* members = enum_scope ? enum_scope->decls : NULL;
                if (members) {
                    for (size_t mi = 0; mi < members->count; mi++) {
                        struct Decl** md = (struct Decl**)vec_get(members, mi);
                        struct Decl* member = md ? *md : NULL;
                        if (!member) continue;
                        bool found = false;
                        for (size_t ci = 0; ci < covered_ids->count; ci++) {
                            uint32_t* id = (uint32_t*)vec_get(covered_ids, ci);
                            if (id && *id == member->name.string_id) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            const char* en_nm = s->pool ? pool_get(s->pool, scrut_t->name_id, 0) : NULL;
                            const char* v_nm  = s->pool ? pool_get(s->pool, member->name.string_id, 0) : NULL;
                            sema_error(s, expr->span,
                                "switch on enum '%s' is not exhaustive: variant '%s' is unhandled",
                                en_nm ? en_nm : "?", v_nm ? v_nm : "?");
                        }
                    }
                }
            }

            result = joined ? joined : s->void_type;
            semantic = SEM_VALUE;
            break;
        }
        case expr_Index: {
            struct Type* obj = sema_infer_expr(s, expr->index.object);
            struct Type* idx = sema_infer_expr(s, expr->index.index);

            if (idx && !sema_type_is_errorish(idx) && !sema_type_is_integer(idx)) {
                char idx_name[128];
                sema_error(s, expr->span,
                    "index expression must be an integer, found %s",
                    sema_type_display_name(s, idx, idx_name, sizeof(idx_name)));
            }

            // Auto-step through optional and one pointer level (`?[]u8` and
            // `^[]u8` both index to `u8`).
            struct Type* probe = obj;
            if (probe && probe->is_optional) {
                probe = sema_unwrap_optional(s, probe);
            }
            if (probe && probe->kind == TYPE_POINTER && probe->elem) {
                probe = probe->elem;
            }

            if (probe && (probe->kind == TYPE_SLICE ||
                          probe->kind == TYPE_ARRAY ||
                          probe->kind == TYPE_STRING)) {
                result = probe->elem ? probe->elem
                       : (probe->kind == TYPE_STRING ? s->u8_type : s->unknown_type);
            } else if (obj && !sema_type_is_errorish(obj)) {
                char obj_name[128];
                sema_error(s, expr->span,
                    "cannot index value of type %s",
                    sema_type_display_name(s, obj, obj_name, sizeof(obj_name)));
                result = s->unknown_type;
            } else {
                result = s->unknown_type;
            }
            semantic = SEM_VALUE;
            break;
        }
        case expr_Loop: {
            sema_infer_expr(s, expr->loop_expr.init);
            struct Type* cond_t = expr->loop_expr.condition
                ? sema_infer_expr(s, expr->loop_expr.condition) : NULL;
            sema_infer_expr(s, expr->loop_expr.step);

            // Capture form `loop (opt) |x|` binds `x` to the unwrapped value
            // of an optional condition. The capture decl was created by the
            // resolver and back-linked via Identifier.resolved.
            if (expr->loop_expr.capture.string_id != 0) {
                bind_optional_capture(s, expr->loop_expr.capture, cond_t,
                    expr->loop_expr.condition->span, "loop");
            } else if (cond_t && !sema_type_is_errorish(cond_t) &&
                       cond_t->kind != TYPE_BOOL && !cond_t->is_optional) {
                // Non-capture form: condition must be bool. Optionals are
                // also OK (truthiness = "is some").
                char name[128];
                sema_error(s, expr->loop_expr.condition->span,
                    "loop condition must be bool or optional, found %s",
                    sema_type_display_name(s, cond_t, name, sizeof(name)));
            }

            result = sema_infer_expr(s, expr->loop_expr.body);
            semantic = SEM_VALUE;
            break;
        }
        case expr_Return: {
            // Look up the enclosing function's declared return type via the
            // active CheckedBody. Return-with-no-value is treated as void.
            struct Type* fn_ret = NULL;
            if (s->current_body && s->current_body->decl) {
                struct Type* fn_t = sema_decl_type(s, s->current_body->decl);
                if (fn_t && fn_t->kind == TYPE_FUNCTION) fn_ret = fn_t->ret;
            }

            if (expr->return_expr.value) {
                if (fn_ret && !sema_type_is_errorish(fn_ret)) {
                    sema_check_expr(s, expr->return_expr.value, fn_ret);
                } else {
                    // No fn_ret available — still walk the value so any
                    // diagnostics inside it fire.
                    sema_infer_expr(s, expr->return_expr.value);
                }
            } else if (fn_ret && !sema_type_is_errorish(fn_ret) &&
                       fn_ret->kind != TYPE_VOID && fn_ret->kind != TYPE_NORETURN) {
                char want[128];
                sema_error(s, expr->span,
                    "return without value but function expects %s",
                    sema_type_display_name(s, fn_ret, want, sizeof(want)));
            }

            // `return` is unreachable past this point in control flow, so
            // the surrounding context uses noreturn-as-bottom join rules.
            // This lets `if (cond) return 0 else <expr>` type as the type
            // of <expr>.
            result = s->noreturn_type;
            semantic = SEM_VALUE;
            break;
        }
        case expr_Defer:
            sema_infer_expr(s, expr->defer_expr.value);
            result = s->void_type;
            semantic = SEM_VALUE;
            break;
        case expr_ArrayType:
            result = infer_array_type(s, expr);
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
            struct Type* size_t_ = sema_infer_expr(s, expr->array_lit.size);
            if (size_t_ && !sema_type_is_errorish(size_t_) &&
                !sema_type_is_integer(size_t_)) {
                char nm[128];
                sema_error(s, expr->span,
                    "array literal size must be an integer, found %s",
                    sema_type_display_name(s, size_t_, nm, sizeof(nm)));
            }

            struct Type* elem_type = sema_infer_type_expr(s, expr->array_lit.elem_type);

            // Initializer is typically a Product literal of N elements.
            // Each element must be assignable to elem_type. The size of the
            // initializer should match the declared size, when both are known.
            struct Expr* init = expr->array_lit.initializer;
            if (init) {
                if (init->kind == expr_Product && init->product.Fields) {
                    Vec* fields = init->product.Fields;
                    for (size_t i = 0; i < fields->count; i++) {
                        struct ProductField* f =
                            (struct ProductField*)vec_get(fields, i);
                        if (!f || !f->value) continue;
                        if (elem_type && !sema_type_is_errorish(elem_type)) {
                            sema_check_expr(s, f->value, elem_type);
                        } else {
                            sema_infer_expr(s, f->value);
                        }
                    }
                } else {
                    // Non-product initializer: just walk it.
                    sema_infer_expr(s, init);
                }
            }
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
            // Control-flow exits don't yield a value; typing them as noreturn
            // (bottom) lets surrounding if-arms join with valued branches the
            // same way `return` does. The resolver already diagnoses
            // break/continue used outside a loop.
            result = s->noreturn_type;
            semantic = SEM_VALUE;
            break;
        case expr_Wildcard:
            // `_` in pattern position matches anything; in expression
            // position (e.g. an `_ = expr` assign target) it's a sink.
            // anytype is assignable to/from any type, so the pattern-match
            // path naturally accepts it without a special case.
            result = s->anytype_type;
            semantic = SEM_VALUE;
            break;
    }

    if (region_id == 0 && result) region_id = result->region_id;
    if (semantic == SEM_UNKNOWN) semantic = sema_semantic_for_type(result);
    sema_record_fact(s, expr, result, semantic, region_id);

    if (comptime_value.kind != CONST_INVALID) {
        sema_record_call_value(s, expr, comptime_value);   // fact now exists
    }

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
                case unary_Optional:
                    return sema_optional_type(s, sema_infer_type_expr(s, expr->unary.operand));
                case unary_Const:
                    // `const T` is currently a no-op qualifier in the type
                    // system; revisit when we add real const-correctness.
                    return sema_infer_type_expr(s, expr->unary.operand);
                default:
                    break;
            }
            break;
        case expr_ArrayType:
            return infer_array_type(s, expr);
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
    // `.Variant` shorthand: needs the expected enum's child_scope to know
    // which variant set to look in. Without context, infer returns unknown
    // — that fall-through path is valid for cases where the surrounding
    // expression doesn't supply a type either, e.g. an isolated discard.
    if (expr->kind == expr_EnumRef && expected && expected->kind == TYPE_ENUM &&
        expected->decl && expected->decl->child_scope) {
        struct Decl* variant = (struct Decl*)hashmap_get(
            &expected->decl->child_scope->name_index,
            (uint64_t)expr->enum_ref_expr.name.string_id);
        if (variant) {
            expr->enum_ref_expr.name.resolved = variant;
            sema_record_fact(s, expr, expected, SEM_VALUE,
                expected->region_id);
            return true;
        }
        const char* en_nm = s->pool ? pool_get(s->pool, expected->name_id, 0) : NULL;
        const char* v_nm  = s->pool ? pool_get(s->pool, expr->enum_ref_expr.name.string_id, 0) : NULL;
        sema_error(s, expr->span,
            "enum '%s' has no variant '%s'",
            en_nm ? en_nm : "?", v_nm ? v_nm : "?");
        // Record an error fact so downstream consumers see this expr
        // exists (just with an errored type).
        sema_record_fact(s, expr, s->error_type, SEM_VALUE, 0);
        return false;
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
