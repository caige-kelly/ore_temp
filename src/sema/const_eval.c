#include "const_eval.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "checker.h"
#include "decls.h"
#include "layout.h"
#include "sema.h"
#include "target.h"
#include "type.h"
#include "../compiler/compiler.h"
#include "../parser/ast.h"

// ----- ConstValue constructors / utilities -----

struct ConstValue sema_const_invalid(void) {
    struct ConstValue v = {0};
    v.kind = CONST_INVALID;
    return v;
}

struct ConstValue sema_const_int(int64_t value) {
    struct ConstValue v = {0};
    v.kind = CONST_INT;
    v.int_val = value;
    return v;
}

struct ConstValue sema_const_float(double value) {
    struct ConstValue v = {0};
    v.kind = CONST_FLOAT;
    v.float_val = value;
    return v;
}

struct ConstValue sema_const_bool(bool value) {
    struct ConstValue v = {0};
    v.kind = CONST_BOOL;
    v.bool_val = value;
    return v;
}

struct ConstValue sema_const_type(struct Type* type) {
    struct ConstValue v = {0};
    v.kind = CONST_TYPE;
    v.type_val = type;
    return v;
}

struct ConstValue sema_const_string(uint32_t string_id) {
    struct ConstValue v = {0};
    v.kind = CONST_STRING;
    v.string_id = string_id;
    return v;
}

struct ConstValue sema_const_void(void) {
    struct ConstValue v = {0};
    v.kind = CONST_VOID;
    return v;
}

bool sema_const_value_is_valid(struct ConstValue value) {
    return value.kind != CONST_INVALID;
}

bool sema_const_value_equal(struct ConstValue a, struct ConstValue b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case CONST_INT:     return a.int_val == b.int_val;
        case CONST_FLOAT:   return a.float_val == b.float_val;
        case CONST_BOOL:    return a.bool_val == b.bool_val;
        case CONST_TYPE:    return a.type_val == b.type_val;
        case CONST_STRING:  return a.string_id == b.string_id;
        case CONST_VOID:    return true;
        case CONST_INVALID: return true;
    }
    return false;
}

// ----- EvalResult -----

struct EvalResult sema_eval_normal(struct ConstValue v) {
    return (struct EvalResult){.control = EVAL_NORMAL, .value = v};
}

struct EvalResult sema_eval_err(void) {
    return (struct EvalResult){.control = EVAL_ERROR, .value = sema_const_invalid()};
}

// ----- ComptimeEnv -----

struct ComptimeEnv* sema_comptime_env_new(struct Sema* s, struct ComptimeEnv* parent) {
    if (!s || !s->arena) return NULL;
    struct ComptimeEnv* env = arena_alloc(s->arena, sizeof(struct ComptimeEnv));
    if (!env) return NULL;
    env->bindings = vec_new_in(s->arena, sizeof(struct ComptimeBinding));
    env->parent = parent;
    return env;
}

void sema_comptime_env_bind(struct Sema* s, struct ComptimeEnv* env, struct Decl* decl, struct ConstValue value) {
    if (!env || !decl) return;
    struct ComptimeCell* cell = arena_alloc(s->arena, sizeof(struct ComptimeCell));
    cell->value = value;
    struct ComptimeBinding b = { .decl = decl, .cell = cell};
    vec_push(env->bindings, &b);
}

bool sema_comptime_env_lookup(struct ComptimeEnv* env, struct Decl* decl, struct ConstValue* out) {
    for (struct ComptimeEnv* cur = env; cur; cur = cur->parent){
        if (!cur->bindings) continue;
        for (size_t i = cur->bindings->count; i > 0; i--) {
            struct ComptimeBinding* b = (struct ComptimeBinding*)vec_get(cur->bindings, i -1);
            if (b && b->decl == decl) {
                if (out) *out = b->cell->value;
                return true;
            }
        }
    }
    return false;
}

void sema_comptime_env_assign(struct Sema* s, struct ComptimeEnv* env, struct Decl* decl, struct ConstValue value) {
    for (struct ComptimeEnv* cur = env; cur; cur = cur->parent) {
        if (!cur->bindings) continue;
        for (size_t i = cur->bindings->count; i > 0; i--) {
            struct ComptimeBinding* b = (struct ComptimeBinding*)vec_get(cur->bindings, i -1 );
            if (b && b->decl == decl) {
                b->cell->value = value;
                return;
            }
        }
    }
}

// ----- helpers -----

static struct ConstValue eval_int_literal(struct Sema* s, struct Expr* expr) {
    const char* text = s->pool ? pool_get(s->pool, expr->lit.string_id, 0) : NULL;
    if (!text || !*text) return sema_const_invalid();

    errno = 0;
    char* end = NULL;
    long long value = strtoll(text, &end, 0);
    if (errno != 0 || (end && *end != '\0')) return sema_const_invalid();
    return sema_const_int((int64_t)value);
}

static struct ConstValue eval_float_literal(struct Sema* s, struct Expr* expr) {
    const char* text = s->pool ? pool_get(s->pool, expr->lit.string_id, 0) : NULL;
    if (!text || !*text) return sema_const_invalid();

    errno = 0;
    char* end = NULL;
    double value = strtod(text, &end);
    if (errno != 0 || (end && *end != '\0')) return sema_const_invalid();
    return sema_const_float(value);
}

static struct EvalResult eval_lit(struct Sema* s, struct Expr* expr) {
    switch (expr->lit.kind) {
        case lit_Int:    return sema_eval_normal(eval_int_literal(s, expr));
        case lit_Float:  return sema_eval_normal(eval_float_literal(s, expr));
        case lit_True:   return sema_eval_normal(sema_const_bool(true));
        case lit_False:  return sema_eval_normal(sema_const_bool(false));
        case lit_String: return sema_eval_normal(sema_const_string(expr->lit.string_id));
        case lit_Byte:   return sema_eval_normal(eval_int_literal(s, expr));
        case lit_Nil:
        default:
            return sema_eval_normal(sema_const_invalid());
    }
}

static bool is_target_field_chain(struct Sema* s, struct Expr* expr,
    const char** out_field) {
    if (!expr || expr->kind != expr_Field) return false;
    struct Expr* obj = expr->field.object;
    if (!obj || obj->kind != expr_Builtin) return false;
    if (!sema_name_is(s, obj->builtin.name_id, "target")) return false;
    if (!s->pool) return false;
    const char* field = pool_get(s->pool, expr->field.field.string_id, 0);
    if (!field) return false;
    if (out_field) *out_field = field;
    return true;
}

static struct TargetInfo target_for(struct Sema* s) {
    if (s && s->compiler) return s->compiler->target;
    return target_default_host();
}

static struct EvalResult eval_target_field(struct Sema* s, const char* field) {
    struct TargetInfo t = target_for(s);
    uint32_t id = 0;
    if (strcmp(field, "os") == 0) {
        const char* name = target_os_name(t.os);
        id = pool_intern(s->pool, name, strlen(name));
        return sema_eval_normal(sema_const_string(id));
    }
    if (strcmp(field, "arch") == 0) {
        const char* name = target_arch_name(t.arch);
        id = pool_intern(s->pool, name, strlen(name));
        return sema_eval_normal(sema_const_string(id));
    }
    if (strcmp(field, "pointer_size") == 0) {
        return sema_eval_normal(sema_const_int((int64_t)t.pointer_size));
    }
    return sema_eval_normal(sema_const_invalid());
}

static struct EvalResult eval_builtin(struct Sema* s, struct Expr* expr, struct ComptimeEnv* env) {
    if (sema_name_is(s, expr->builtin.name_id, "sizeOf") ||
        sema_name_is(s, expr->builtin.name_id, "alignOf")) {
        bool is_size = sema_name_is(s, expr->builtin.name_id, "sizeOf");
        if (!expr->builtin.args || expr->builtin.args->count == 0) return sema_eval_normal(sema_const_invalid());

        struct Expr** arg_p = (struct Expr**)vec_get(expr->builtin.args, 0);
        struct Expr* arg = arg_p ? *arg_p : NULL;
        if (!arg) return sema_eval_normal(sema_const_invalid());

        struct Type* arg_type = NULL;
        if (arg->kind == expr_Ident) {
            struct EvalResult cv = sema_const_eval_expr(s, arg, env);
            if (cv.control != EVAL_NORMAL) return cv;
            if (cv.value.kind == CONST_TYPE) arg_type = cv.value.type_val;
        }
        if (!arg_type) arg_type = sema_infer_type_expr(s, arg);
        if (!arg_type || sema_type_is_errorish(arg_type)) return sema_eval_normal(sema_const_invalid());

        struct TypeLayout layout = sema_layout_of_type_at(s, arg_type, expr->span);
        if (!layout.complete) return sema_eval_normal(sema_const_invalid());

        return sema_eval_normal(sema_const_int((int64_t)(is_size ? layout.size : layout.align)));
    }

    if (sema_name_is(s, expr->builtin.name_id, "target")) {
        // bare @target is not a value — it's only meaningful via field access.
        return sema_eval_normal(sema_const_invalid());
    }

    return sema_eval_normal(sema_const_invalid());
}

static struct EvalResult eval_ident(struct Sema* s, struct Expr* expr,
    struct ComptimeEnv* env) {
    struct Decl* decl = expr->ident.resolved;
    if (!decl) return sema_eval_normal(sema_const_invalid());

    struct ConstValue env_val;
    if (sema_comptime_env_lookup(env, decl, &env_val)) return sema_eval_normal(env_val);

    if (decl->semantic_kind == SEM_TYPE) {
        struct Type* t = sema_type_of_decl(s, decl);
        if (!t || sema_type_is_errorish(t)) return sema_eval_normal(sema_const_invalid());
        return sema_eval_normal(sema_const_type(t));
    }

    if (decl->kind == DECL_PRIMITIVE) {
        if (sema_name_is(s, decl->name.string_id, "true"))  return sema_eval_normal(sema_const_bool(true));
        if (sema_name_is(s, decl->name.string_id, "false")) return sema_eval_normal(sema_const_bool(false));
    }

    if (decl->semantic_kind == SEM_VALUE && decl->node && decl->node->kind == expr_Bind) {
        if (decl->node->bind.value) {
            return sema_const_eval_expr(s, decl->node->bind.value, env);
        }
    }

    return sema_eval_normal(sema_const_invalid());
}

static struct EvalResult eval_bin(struct Sema* s, struct Expr* expr, struct ComptimeEnv* env) {
    struct EvalResult lr = sema_const_eval_expr(s, expr->bin.Left, env);
    if (lr.control != EVAL_NORMAL) return lr;

    struct EvalResult rr = sema_const_eval_expr(s, expr->bin.Right, env);
    if (rr.control != EVAL_NORMAL) return rr;

    struct ConstValue l = lr.value;
    struct ConstValue r = rr.value;
    if (l.kind == CONST_INVALID || r.kind == CONST_INVALID)
        return sema_eval_normal(sema_const_invalid());

    if (l.kind == CONST_FLOAT && r.kind == CONST_FLOAT) {
        switch (expr->bin.op) {
            case Plus:         return sema_eval_normal(sema_const_float(l.float_val + r.float_val));
            case Minus:        return sema_eval_normal(sema_const_float(l.float_val - r.float_val));
            case Star:         return sema_eval_normal(sema_const_float(l.float_val * r.float_val));
            case ForwardSlash: if (r.float_val == 0.0) return sema_eval_normal(sema_const_invalid());
                               return sema_eval_normal(sema_const_float(l.float_val / r.float_val));
            case EqualEqual:   return sema_eval_normal(sema_const_bool(l.float_val == r.float_val));
            case BangEqual:    return sema_eval_normal(sema_const_bool(l.float_val != r.float_val));
            case Less:         return sema_eval_normal(sema_const_bool(l.float_val < r.float_val));
            case LessEqual:    return sema_eval_normal(sema_const_bool(l.float_val <= r.float_val));
            case Greater:      return sema_eval_normal(sema_const_bool(l.float_val > r.float_val));
            case GreaterEqual: return sema_eval_normal(sema_const_bool(l.float_val >= r.float_val));
            default: break;
        }
        return sema_eval_normal(sema_const_invalid());
    }

    if (l.kind == CONST_INT && r.kind == CONST_INT) {
        switch (expr->bin.op) {
            case Plus:        return sema_eval_normal(sema_const_int(l.int_val + r.int_val));
            case Minus:       return sema_eval_normal(sema_const_int(l.int_val - r.int_val));
            case Star:        return sema_eval_normal(sema_const_int(l.int_val * r.int_val));
            case ForwardSlash: if (r.int_val == 0) return sema_eval_normal(sema_const_invalid());
                              return sema_eval_normal(sema_const_int(l.int_val / r.int_val));
            case Percent:     if (r.int_val == 0) return sema_eval_normal(sema_const_invalid());
                              return sema_eval_normal(sema_const_int(l.int_val % r.int_val));
            case Ampersand:   return sema_eval_normal(sema_const_int(l.int_val & r.int_val));
            case Pipe:        return sema_eval_normal(sema_const_int(l.int_val | r.int_val));
            case Caret:       return sema_eval_normal(sema_const_int(l.int_val ^ r.int_val));
            case ShiftLeft:   return sema_eval_normal(sema_const_int(l.int_val << r.int_val));
            case ShiftRight:  return sema_eval_normal(sema_const_int(l.int_val >> r.int_val));
            case EqualEqual:   return sema_eval_normal(sema_const_bool(l.int_val == r.int_val));
            case BangEqual:    return sema_eval_normal(sema_const_bool(l.int_val != r.int_val));
            case Less:         return sema_eval_normal(sema_const_bool(l.int_val < r.int_val));
            case LessEqual:    return sema_eval_normal(sema_const_bool(l.int_val <= r.int_val));
            case Greater:      return sema_eval_normal(sema_const_bool(l.int_val > r.int_val));
            case GreaterEqual: return sema_eval_normal(sema_const_bool(l.int_val >= r.int_val));
            default: break;
        }
    }

    if (l.kind == CONST_BOOL && r.kind == CONST_BOOL) {
        switch (expr->bin.op) {
            case AmpersandAmpersand: return sema_eval_normal(sema_const_bool(l.bool_val && r.bool_val));
            case PipePipe:           return sema_eval_normal(sema_const_bool(l.bool_val || r.bool_val));
            case EqualEqual:         return sema_eval_normal(sema_const_bool(l.bool_val == r.bool_val));
            case BangEqual:          return sema_eval_normal(sema_const_bool(l.bool_val != r.bool_val));
            default: break;
        }
    }

    if (l.kind == CONST_STRING && r.kind == CONST_STRING) {
        switch (expr->bin.op) {
            case EqualEqual: return sema_eval_normal(sema_const_bool(l.string_id == r.string_id));
            case BangEqual:  return sema_eval_normal(sema_const_bool(l.string_id != r.string_id));
            default: break;
        }
    }

    return sema_eval_normal(sema_const_invalid());
}

static struct EvalResult eval_unary(struct Sema* s, struct Expr* expr, struct ComptimeEnv* env) { 
    struct EvalResult v = sema_const_eval_expr(s, expr->unary.operand, env);
    if (v.control != EVAL_NORMAL) return v;
    if (v.value.kind == CONST_INVALID) return sema_eval_normal(sema_const_invalid());

    switch (expr->unary.op) {
        case unary_Neg:
            if (v.value.kind == CONST_INT) return sema_eval_normal(sema_const_int(-v.value.int_val));
            if (v.value.kind == CONST_FLOAT) return sema_eval_normal(sema_const_float(-v.value.float_val));
            return sema_eval_normal(sema_const_invalid());
        case unary_BitNot:
            if (v.value.kind == CONST_INT) return sema_eval_normal(sema_const_int(~v.value.int_val));
            return sema_eval_normal(sema_const_invalid());
        case unary_Not:
            if (v.value.kind == CONST_BOOL) return sema_eval_normal(sema_const_bool(!v.value.bool_val));
            return sema_eval_normal(sema_const_invalid());
        default:
            return sema_eval_normal(sema_const_invalid());
    }
}

static struct EvalResult eval_block(struct Sema* s, struct Expr* expr, struct ComptimeEnv* env) {
    if (expr-> block.stmts->count == 0) {
        return sema_eval_normal(sema_const_void());
    }

    struct ComptimeEnv* block_env = sema_comptime_env_new(s, env);
    
    struct EvalResult last = sema_eval_normal(sema_const_void());
    for (size_t i = 0; i < expr->block.stmts->count; i++) {
        struct Expr** stmt_p = (struct Expr**)vec_get(expr->block.stmts, i);
        struct Expr* stmt = stmt_p ? *stmt_p : NULL;
        if (!stmt) continue;

        last = sema_const_eval_expr(s, stmt, block_env);
        if (last.control != EVAL_NORMAL) return last;
    }
    return last;
}

struct EvalResult sema_const_eval_expr(struct Sema* s, struct Expr* expr,
    struct ComptimeEnv* env) {
    if (!s || !expr) return sema_eval_normal(sema_const_invalid());

    switch (expr->kind) {
        case expr_Lit:
            return eval_lit(s, expr);
        case expr_Ident:
            return eval_ident(s, expr, env);
        case expr_Bin:
            return eval_bin(s, expr, env);
        case expr_Unary:
            return eval_unary(s, expr, env);
        case expr_Builtin:
            return eval_builtin(s, expr, env);
        case expr_Field: {
            const char* field = NULL;
            if (is_target_field_chain(s, expr, &field)) {
                return eval_target_field(s, field);
            }
            return sema_eval_normal(sema_const_invalid());
        }
        case expr_Bind:
            return sema_const_eval_expr(s, expr->bind.value, env);
        case expr_Block:
            return eval_block(s, expr, env);
        default:
            return sema_eval_normal(sema_const_invalid());
    }
}
