#include "../../sema.h"
#include "../../../diag/diag.h"
#include <math.h>

struct ConstValue bin_add(struct Sema *s, struct Expr *expr, struct ConstValue l, struct ConstValue r) {

    if (l.kind == CONST_INT && r.kind == CONST_INT) {
        int64_t v;
        if (__builtin_add_overflow(l.int_val, r.int_val, &v)) {
            diag_error(&s->diags, expr->span, "int overflow during addition");
            return (struct ConstValue){.kind = CONST_NONE};
        }
        return (struct ConstValue){.kind = CONST_INT, .int_val = v};
    }
    if (l.kind == CONST_FLOAT && r.kind == CONST_FLOAT) {
        double result = l.float_val + r.float_val;

        // isfinite returns false if the result is Infinity or NaN
        if (!isfinite(result)) {
            diag_error(&s->diags, expr->span, "float overflow during addition");
            return (struct ConstValue){.kind = CONST_NONE};
        }

        return (struct ConstValue){.kind = CONST_FLOAT, .float_val = result};
    }
    return (struct ConstValue){.kind = CONST_NONE};
}

struct ConstValue bin_sub(struct Sema *s, struct Expr *expr, struct ConstValue l, struct ConstValue r) {
    if (l.kind == CONST_INT && r.kind == CONST_INT) {
        int64_t v;
        if (__builtin_sub_overflow(l.int_val, r.int_val, &v)) {
            diag_error(&s->diags, expr->span, "int overflow during subtraction");
            return (struct ConstValue){.kind = CONST_NONE};
        }
        return (struct ConstValue){.kind = CONST_INT, .int_val = v};
    }
    if (l.kind == CONST_FLOAT && r.kind == CONST_FLOAT) {
        double result = l.float_val - r.float_val;
        if (!isfinite(result)) {
            diag_error(&s->diags, expr->span, "float overflow during subtraction");
            return (struct ConstValue){.kind = CONST_NONE};
        }
        return (struct ConstValue){.kind = CONST_FLOAT, .float_val = result};
    }
    return (struct ConstValue){.kind = CONST_NONE};
}

struct ConstValue bin_mul(struct Sema *s, struct Expr *expr, struct ConstValue l, struct ConstValue r) {
    if (l.kind == CONST_INT && r.kind == CONST_INT) {
        int64_t v;
        if (__builtin_mul_overflow(l.int_val, r.int_val, &v)) {
            diag_error(&s->diags, expr->span, "int overflow during multiplication");
            return (struct ConstValue){.kind = CONST_NONE};
        }
        return (struct ConstValue){.kind = CONST_INT, .int_val = v};
    }
    if (l.kind == CONST_FLOAT && r.kind == CONST_FLOAT) {
        double result = l.float_val * r.float_val;
        if (!isfinite(result)) {
            diag_error(&s->diags, expr->span, "float overflow during multiplication");
            return (struct ConstValue){.kind = CONST_NONE};
        }
        return (struct ConstValue){.kind = CONST_FLOAT, .float_val = result};
    }
    return (struct ConstValue){.kind = CONST_NONE};
}

struct ConstValue bin_div(struct Sema *s, struct Expr *expr, struct ConstValue l, struct ConstValue r) {
    if (l.kind == CONST_INT && r.kind == CONST_INT) {
        if (r.int_val == 0) {
            diag_error(&s->diags, expr->span, "division by zero in comptime expression");
            return (struct ConstValue){.kind = CONST_NONE};
        }
        // INT64_MIN / -1 overflows. Match the unary-neg guard's treatment.
        if (l.int_val == INT64_MIN && r.int_val == -1) {
            diag_error(&s->diags, expr->span, "int overflow during division");
            return (struct ConstValue){.kind = CONST_NONE};
        }
        return (struct ConstValue){.kind = CONST_INT, .int_val = l.int_val / r.int_val};
    }
    if (l.kind == CONST_FLOAT && r.kind == CONST_FLOAT) {
        double result = l.float_val / r.float_val;
        if (!isfinite(result)) {
            diag_error(&s->diags, expr->span, "float overflow / divide-by-zero in comptime expression");
            return (struct ConstValue){.kind = CONST_NONE};
        }
        return (struct ConstValue){.kind = CONST_FLOAT, .float_val = result};
    }
    return (struct ConstValue){.kind = CONST_NONE};
}

struct ConstValue bin_mod(struct Sema *s, struct Expr *expr, struct ConstValue l, struct ConstValue r) {
    // Modulo only defined on integers in this lattice.
    if (l.kind != CONST_INT || r.kind != CONST_INT)
        return (struct ConstValue){.kind = CONST_NONE};
    if (r.int_val == 0) {
        diag_error(&s->diags, expr->span, "modulo by zero in comptime expression");
        return (struct ConstValue){.kind = CONST_NONE};
    }
    if (l.int_val == INT64_MIN && r.int_val == -1) {
        // Same UB-edge as div.
        return (struct ConstValue){.kind = CONST_INT, .int_val = 0};
    }
    return (struct ConstValue){.kind = CONST_INT, .int_val = l.int_val % r.int_val};
}