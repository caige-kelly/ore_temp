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