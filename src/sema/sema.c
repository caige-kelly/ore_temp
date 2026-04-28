#include "sema.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static struct Type* sema_type_new(struct Sema* s, TypeKind kind) {
    struct Type* t = arena_alloc(s->arena, sizeof(struct Type));
    t->kind = kind;
    t->params = vec_new_in(s->arena, sizeof(struct Type*));
    t->effects = vec_new_in(s->arena, sizeof(struct EffectSig*));
    return t;
}

static struct Type* sema_named_type(struct Sema* s, TypeKind kind, uint32_t name_id, struct Decl* decl) {
    struct Type* t = sema_type_new(s, kind);
    t->name_id = name_id;
    t->decl = decl;
    return t;
}

static void sema_error(struct Sema* s, struct Span span, const char* fmt, ...) {
    struct SemaError err = {0};
    err.span = span;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err.msg, sizeof(err.msg), fmt, ap);
    va_end(ap);
    vec_push(s->errors, &err);
    if (s->diags) {
        diag_error(s->diags, span, "%s", err.msg);
    }
    s->has_errors = true;
}

static const char* type_kind_str(TypeKind kind) {
    switch (kind) {
        case TYPE_UNKNOWN:     return "unknown";
        case TYPE_ERROR:       return "error";
        case TYPE_VOID:        return "void";
        case TYPE_BOOL:        return "bool";
        case TYPE_INT:         return "int";
        case TYPE_FLOAT:       return "float";
        case TYPE_STRING:      return "string";
        case TYPE_NIL:         return "nil";
        case TYPE_TYPE:        return "type";
        case TYPE_ANYTYPE:     return "anytype";
        case TYPE_MODULE:      return "module";
        case TYPE_STRUCT:      return "struct";
        case TYPE_ENUM:        return "enum";
        case TYPE_EFFECT:      return "effect";
        case TYPE_EFFECT_ROW:  return "effect-row";
        case TYPE_SCOPE_TOKEN: return "scope-token";
        case TYPE_FUNCTION:    return "function";
        case TYPE_POINTER:     return "pointer";
        case TYPE_SLICE:       return "slice";
        case TYPE_ARRAY:       return "array";
        case TYPE_PRODUCT:     return "product";
    }
    return "?";
}

static const char* semantic_kind_str(SemanticKind kind) {
    switch (kind) {
        case SEM_UNKNOWN:     return "unknown";
        case SEM_VALUE:       return "value";
        case SEM_TYPE:        return "type";
        case SEM_EFFECT:      return "effect";
        case SEM_MODULE:      return "module";
        case SEM_SCOPE_TOKEN: return "scope-token";
        case SEM_EFFECT_ROW:  return "effect-row";
    }
    return "?";
}

static bool name_is(struct Sema* s, uint32_t id, const char* name) {
    const char* got = pool_get(s->pool, id, 0);
    return got && strcmp(got, name) == 0;
}

static struct Type* primitive_type_for_name(struct Sema* s, uint32_t id) {
    if (name_is(s, id, "void")) return s->void_type;
    if (name_is(s, id, "bool")) return s->bool_type;
    if (name_is(s, id, "type")) return s->type_type;
    if (name_is(s, id, "anytype")) return s->anytype_type;
    if (name_is(s, id, "Scope")) return s->type_type;
    if (name_is(s, id, "true") || name_is(s, id, "false")) return s->bool_type;
    if (name_is(s, id, "nil")) return s->nil_type;

    const char* nm = pool_get(s->pool, id, 0);
    if (!nm) return s->unknown_type;
    if (nm[0] == 'i' || nm[0] == 'u') return s->int_type;
    if (nm[0] == 'f') return s->float_type;
    return s->unknown_type;
}

static SemanticKind semantic_for_type(struct Type* type) {
    if (!type) return SEM_UNKNOWN;
    switch (type->kind) {
        case TYPE_MODULE:      return SEM_MODULE;
        case TYPE_STRUCT:
        case TYPE_ENUM:
        case TYPE_TYPE:
        case TYPE_ANYTYPE:     return SEM_TYPE;
        case TYPE_EFFECT:      return SEM_EFFECT;
        case TYPE_EFFECT_ROW:  return SEM_EFFECT_ROW;
        case TYPE_SCOPE_TOKEN: return SEM_SCOPE_TOKEN;
        case TYPE_UNKNOWN:
        case TYPE_ERROR:       return SEM_UNKNOWN;
        default:               return SEM_VALUE;
    }
}

static void record_fact(struct Sema* s, struct Expr* expr, struct Type* type, SemanticKind semantic_kind, uint32_t region_id) {
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

static struct Type* infer_expr(struct Sema* s, struct Expr* expr);
static struct Type* infer_type_expr(struct Sema* s, struct Expr* expr);

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

static void effect_sig_collect_term(struct Sema* s, struct EffectSig* sig, struct Expr* expr) {
    if (!expr) return;

    switch (expr->kind) {
        case expr_EffectRow:
            effect_sig_collect_term(s, sig, expr->effect_row.head);
            effect_sig_push_row(sig, expr->effect_row.row);
            return;
        case expr_Bin:
            if (expr->bin.op == Pipe) {
                effect_sig_collect_term(s, sig, expr->bin.Left);
                effect_sig_collect_term(s, sig, expr->bin.Right);
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

static struct EffectSig* effect_sig_from_expr(struct Sema* s, struct Expr* effect) {
    if (!effect) return NULL;
    struct EffectSig* existing = effect_sig_find_existing(s, effect);
    if (existing) return existing;

    struct EffectSig* sig = effect_sig_new(s, effect);
    effect_sig_collect_term(s, sig, effect);
    vec_push(s->effect_sigs, &sig);
    return sig;
}

static struct Type* type_from_decl(struct Sema* s, struct Decl* decl) {
    if (!decl) return s->unknown_type;

    switch (decl->semantic_kind) {
        case SEM_MODULE:
            return sema_named_type(s, TYPE_MODULE, decl->name.string_id, decl);
        case SEM_EFFECT:
            return sema_named_type(s, TYPE_EFFECT, decl->name.string_id, decl);
        case SEM_SCOPE_TOKEN: {
            struct Type* t = sema_named_type(s, TYPE_SCOPE_TOKEN, decl->name.string_id, decl);
            t->region_id = decl->scope_token_id;
            return t;
        }
        case SEM_EFFECT_ROW:
            return sema_named_type(s, TYPE_EFFECT_ROW, decl->name.string_id, decl);
        case SEM_TYPE:
            if (decl->kind == DECL_PRIMITIVE) return primitive_type_for_name(s, decl->name.string_id);
            if (decl->node && decl->node->kind == expr_Bind && decl->node->bind.value) {
                switch (decl->node->bind.value->kind) {
                    case expr_Struct: return sema_named_type(s, TYPE_STRUCT, decl->name.string_id, decl);
                    case expr_Enum:   return sema_named_type(s, TYPE_ENUM, decl->name.string_id, decl);
                    default: break;
                }
            }
            return sema_named_type(s, TYPE_TYPE, decl->name.string_id, decl);
        case SEM_VALUE:
            if (decl->kind == DECL_PRIMITIVE) return primitive_type_for_name(s, decl->name.string_id);
            if (decl->node && decl->node->kind == expr_Bind && decl->node->bind.value &&
                decl->node->bind.value->kind == expr_Lambda) {
                return infer_expr(s, decl->node->bind.value);
            }
            return s->unknown_type;
        case SEM_UNKNOWN:
        default:
            return s->unknown_type;
    }
}

static struct Type* infer_lit(struct Sema* s, struct Expr* expr) {
    switch (expr->lit.kind) {
        case lit_Int:    return s->int_type;
        case lit_Float:  return s->float_type;
        case lit_String: return s->string_type;
        case lit_Byte:   return s->int_type;
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

static struct Type* infer_function_like(struct Sema* s, Vec* params, struct Expr* ret_type, struct Expr* effect, struct Expr* body) {
    struct Type* fn = sema_type_new(s, TYPE_FUNCTION);
    if (params) {
        for (size_t i = 0; i < params->count; i++) {
            struct Param* p = (struct Param*)vec_get(params, i);
            struct Type* pt = p ? infer_type_expr(s, p->type_ann) : s->unknown_type;
            vec_push(fn->params, &pt);
        }
    }
    if (effect) {
        struct EffectSig* sig = effect_sig_from_expr(s, effect);
        fn->effect_sig = sig;
        if (sig) vec_push(fn->effects, &sig);
        infer_expr(s, effect);
    }
    fn->ret = ret_type ? infer_type_expr(s, ret_type) : infer_expr(s, body);
    return fn;
}

static struct Type* infer_expr(struct Sema* s, struct Expr* expr) {
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
            struct Decl* d = expr->ident.resolved;
            result = type_from_decl(s, d);
            semantic = d ? d->semantic_kind : SEM_UNKNOWN;
            region_id = result ? result->region_id : 0;
            break;
        }
        case expr_Field: {
            infer_expr(s, expr->field.object);
            struct Decl* d = expr->field.field.resolved;
            result = type_from_decl(s, d);
            semantic = d ? d->semantic_kind : semantic_for_type(result);
            region_id = result ? result->region_id : 0;
            break;
        }
        case expr_Builtin:
            if (name_is(s, expr->builtin.name_id, "import")) {
                result = s->module_type;
                semantic = SEM_MODULE;
            } else if (name_is(s, expr->builtin.name_id, "sizeOf") ||
                name_is(s, expr->builtin.name_id, "alignOf") ||
                name_is(s, expr->builtin.name_id, "intCast")) {
                result = s->int_type;
                semantic = SEM_VALUE;
            } else if (name_is(s, expr->builtin.name_id, "TypeOf")) {
                result = s->type_type;
                semantic = SEM_TYPE;
            } else {
                result = s->unknown_type;
                semantic = SEM_VALUE;
            }
            if (expr->builtin.args) {
                for (size_t i = 0; i < expr->builtin.args->count; i++) {
                    struct Expr** arg = (struct Expr**)vec_get(expr->builtin.args, i);
                    if (arg) infer_expr(s, *arg);
                }
            }
            break;
        case expr_Bin: {
            struct Type* left = infer_expr(s, expr->bin.Left);
            infer_expr(s, expr->bin.Right);
            result = op_is_comparison(expr->bin.op) ? s->bool_type : left;
            semantic = SEM_VALUE;
            break;
        }
        case expr_Assign:
            infer_expr(s, expr->assign.target);
            result = infer_expr(s, expr->assign.value);
            semantic = SEM_VALUE;
            break;
        case expr_Unary: {
            struct Type* inner = infer_expr(s, expr->unary.operand);
            if (expr->unary.op == unary_Ref || expr->unary.op == unary_Ptr) {
                result = sema_type_new(s, TYPE_POINTER);
                result->elem = inner;
                // Region propagation is deliberately only a slot for now.
                // Borrow-lite will later color this from scoped resources.
                result->region_id = inner ? inner->region_id : 0;
                region_id = result->region_id;
            } else {
                result = inner;
            }
            semantic = SEM_VALUE;
            break;
        }
        case expr_Call: {
            struct Type* callee = infer_expr(s, expr->call.callee);
            if (expr->call.args) {
                for (size_t i = 0; i < expr->call.args->count; i++) {
                    struct Expr** arg = (struct Expr**)vec_get(expr->call.args, i);
                    if (arg) infer_expr(s, *arg);
                }
            }
            if (callee && callee->kind == TYPE_FUNCTION && callee->ret) {
                result = callee->ret;
            } else if (callee && callee->kind == TYPE_EFFECT) {
                result = callee;
                semantic = SEM_EFFECT;
                break;
            } else {
                result = s->unknown_type;
            }
            semantic = SEM_VALUE;
            break;
        }
        case expr_EffectRow:
            infer_expr(s, expr->effect_row.head);
            if (expr->effect_row.row.resolved &&
                expr->effect_row.row.resolved->semantic_kind != SEM_EFFECT_ROW) {
                const char* nm = pool_get(s->pool, expr->effect_row.row.string_id, 0);
                sema_error(s, expr->effect_row.row.span,
                    "'%s' is not an effect-row variable", nm ? nm : "?");
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
                    struct StructMember* m = (struct StructMember*)vec_get(expr->struct_expr.members, i);
                    if (!m) continue;
                    if (m->kind == member_Field) {
                        infer_type_expr(s, m->field.type);
                        infer_expr(s, m->field.default_value);
                    } else if (m->kind == member_Union && m->union_def.variants) {
                        for (size_t j = 0; j < m->union_def.variants->count; j++) {
                            struct FieldDef* f = (struct FieldDef*)vec_get(m->union_def.variants, j);
                            if (!f) continue;
                            infer_type_expr(s, f->type);
                            infer_expr(s, f->default_value);
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
                    struct EnumVariant* v = (struct EnumVariant*)vec_get(expr->enum_expr.variants, i);
                    if (v) infer_expr(s, v->explicit_value);
                }
            }
            result = sema_type_new(s, TYPE_ENUM);
            semantic = SEM_TYPE;
            break;
        case expr_Effect:
            if (expr->effect_expr.operations) {
                for (size_t i = 0; i < expr->effect_expr.operations->count; i++) {
                    struct Expr** op = (struct Expr**)vec_get(expr->effect_expr.operations, i);
                    if (op) infer_expr(s, *op);
                }
            }
            result = s->effect_type;
            semantic = SEM_EFFECT;
            break;
        case expr_Bind:
            infer_type_expr(s, expr->bind.type_ann);
            result = infer_expr(s, expr->bind.value);
            semantic = semantic_for_type(result);
            break;
        case expr_DestructureBind:
            result = infer_expr(s, expr->destructure.value);
            semantic = SEM_VALUE;
            break;
        case expr_Block: {
            result = s->void_type;
            Vec* stmts = &expr->block.stmts;
            for (size_t i = 0; i < stmts->count; i++) {
                struct Expr** stmt = (struct Expr**)vec_get(stmts, i);
                if (stmt && *stmt) result = infer_expr(s, *stmt);
            }
            semantic = SEM_VALUE;
            break;
        }
        case expr_Product:
            infer_type_expr(s, expr->product.type_expr);
            if (expr->product.Fields) {
                for (size_t i = 0; i < expr->product.Fields->count; i++) {
                    struct ProductField* f = (struct ProductField*)vec_get(expr->product.Fields, i);
                    if (f) infer_expr(s, f->value);
                }
            }
            result = sema_type_new(s, TYPE_PRODUCT);
            semantic = SEM_VALUE;
            break;
        case expr_If:
            infer_expr(s, expr->if_expr.condition);
            result = infer_expr(s, expr->if_expr.then_branch);
            infer_expr(s, expr->if_expr.else_branch);
            semantic = SEM_VALUE;
            break;
        case expr_For:
            infer_expr(s, expr->for_expr.iter);
            infer_expr(s, expr->for_expr.where_clause);
            result = infer_expr(s, expr->for_expr.body);
            semantic = SEM_VALUE;
            break;
        case expr_Switch:
            infer_expr(s, expr->switch_expr.scrutinee);
            if (expr->switch_expr.arms) {
                for (size_t i = 0; i < expr->switch_expr.arms->count; i++) {
                    struct SwitchArm* arm = (struct SwitchArm*)vec_get(expr->switch_expr.arms, i);
                    if (!arm) continue;
                    if (arm->patterns) {
                        for (size_t j = 0; j < arm->patterns->count; j++) {
                            struct Expr** pat = (struct Expr**)vec_get(arm->patterns, j);
                            if (pat) infer_expr(s, *pat);
                        }
                    }
                    result = infer_expr(s, arm->body);
                }
            }
            semantic = SEM_VALUE;
            break;
        case expr_With:
            infer_expr(s, expr->with.func);
            result = infer_expr(s, expr->with.body);
            semantic = SEM_VALUE;
            break;
        case expr_Index:
            infer_expr(s, expr->index.object);
            infer_expr(s, expr->index.index);
            result = s->unknown_type;
            semantic = SEM_VALUE;
            break;
        case expr_Loop:
            infer_expr(s, expr->loop_expr.init);
            infer_expr(s, expr->loop_expr.condition);
            infer_expr(s, expr->loop_expr.step);
            result = infer_expr(s, expr->loop_expr.body);
            semantic = SEM_VALUE;
            break;
        case expr_Return:
            result = infer_expr(s, expr->return_expr.value);
            semantic = SEM_VALUE;
            break;
        case expr_Defer:
            infer_expr(s, expr->defer_expr.value);
            result = s->void_type;
            semantic = SEM_VALUE;
            break;
        case expr_ArrayType:
            infer_expr(s, expr->array_type.size);
            result = sema_type_new(s, expr->array_type.is_many_ptr ? TYPE_POINTER : TYPE_ARRAY);
            result->elem = infer_type_expr(s, expr->array_type.elem);
            semantic = SEM_TYPE;
            break;
        case expr_SliceType:
            result = sema_type_new(s, TYPE_SLICE);
            result->elem = infer_type_expr(s, expr->slice_type.elem);
            semantic = SEM_TYPE;
            break;
        case expr_ManyPtrType:
            result = sema_type_new(s, TYPE_POINTER);
            result->elem = infer_type_expr(s, expr->many_ptr_type.elem);
            semantic = SEM_TYPE;
            break;
        case expr_ArrayLit:
            infer_expr(s, expr->array_lit.size);
            infer_type_expr(s, expr->array_lit.elem_type);
            infer_expr(s, expr->array_lit.initializer);
            result = sema_type_new(s, TYPE_ARRAY);
            semantic = SEM_VALUE;
            break;
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
    if (semantic == SEM_UNKNOWN) semantic = semantic_for_type(result);
    record_fact(s, expr, result, semantic, region_id);
    return result;
}

static struct Type* infer_type_expr(struct Sema* s, struct Expr* expr) {
    if (!expr) return s->unknown_type;
    struct Type* t = infer_expr(s, expr);
    // This pass is deliberately permissive: dependent/comptime type
    // params are still values today. Full type checking will tighten this.
    return t ? t : s->unknown_type;
}

struct Sema sema_new(struct Resolver* resolver, StringPool* pool, Arena* arena,
                     struct DiagBag* diags) {
    struct Sema s = {0};
    s.arena = arena;
    s.pool = pool;
    s.resolver = resolver;
    s.diags = diags;
    s.facts = vec_new_in(arena, sizeof(struct SemaFact));
    s.effect_sigs = vec_new_in(arena, sizeof(struct EffectSig*));
    s.errors = vec_new_in(arena, sizeof(struct SemaError));
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

    if (s->resolver->modules) {
        for (size_t i = 0; i < s->resolver->modules->count; i++) {
            struct Module** mod_p = (struct Module**)vec_get(s->resolver->modules, i);
            if (!mod_p || !*mod_p || !(*mod_p)->ast) continue;
            Vec* ast = (*mod_p)->ast;
            for (size_t j = 0; j < ast->count; j++) {
                struct Expr** expr = (struct Expr**)vec_get(ast, j);
                if (expr && *expr) infer_expr(s, *expr);
            }
        }
    } else if (s->resolver->ast) {
        for (size_t i = 0; i < s->resolver->ast->count; i++) {
            struct Expr** expr = (struct Expr**)vec_get(s->resolver->ast, i);
            if (expr && *expr) infer_expr(s, *expr);
        }
    }

    return !s->has_errors;
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

static void print_effect_sig(struct Sema* s, struct EffectSig* sig) {
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

void dump_sema(struct Sema* s) {
    if (!s) return;
    printf("\n=== sema skeleton ===\n");
    printf("  facts:  %zu\n", s->facts ? s->facts->count : 0);
    printf("  effect sigs: %zu\n", s->effect_sigs ? s->effect_sigs->count : 0);
    printf("  errors: %zu\n", s->errors ? s->errors->count : 0);

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
        printf("    %-12s %zu\n", type_kind_str((TypeKind)i), counts[i]);
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
            print_effect_sig(s, sig);
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
                semantic_kind_str(fact->semantic_kind),
                type_kind_str(fact->type->kind));
            if (fact->region_id) printf(" @region#%u", fact->region_id);
            printf("\n");
            shown++;
        }
    }

    if (s->errors && s->errors->count > 0) {
        printf("\n=== sema errors ===\n");
        for (size_t i = 0; i < s->errors->count; i++) {
            struct SemaError* err = (struct SemaError*)vec_get(s->errors, i);
            if (!err) continue;
            printf("  line %d col %d: %s\n", err->span.line, err->span.column, err->msg);
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
        print_effect_sig(s, sig);
        if (sig->row_decl && sig->row_decl->semantic_kind == SEM_EFFECT_ROW) {
            const char* row_name = pool_get(s->pool, sig->row_name_id, 0);
            printf("  open-row=%s", row_name ? row_name : "?");
        }
        printf("\n");
    }
}
