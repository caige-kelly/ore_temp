#include "bin_ops.h"
#include <math.h>

struct ConstValue bin_add(struct ConstValue l, struct ConstValue r) {

    if (l.kind == CONST_INT && r.kind == CONST_INT) {
        int64_t v;
        if (__builtin_add_overflow(l.int_val, r.int_val, &v)) {
            // overflow — return CONST_NONE, optionally emit a diagnostic
            return (struct ConstValue){.kind = CONST_NONE};
        }
        return (struct ConstValue){.kind = CONST_INT, .int_val = v};
    }
    if (l.kind == CONST_FLOAT && r.kind == CONST_FLOAT) {
        double result = l.float_val + r.float_val;
        
        // isfinite returns false if the result is Infinity or NaN
        if (!isfinite(result)) {
            return (struct ConstValue){.kind = CONST_NONE};
        }
        
        return (struct ConstValue){.kind = CONST_FLOAT, .float_val = result};
    }
    return (struct ConstValue){.kind = CONST_NONE};
}