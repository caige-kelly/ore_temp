#include "bin_ops.h"

struct ConstValue bin_add(struct ConstValue l, struct ConstValue r) {
    if (l.kind == CONST_INT) {
        l.int_val += r.int_val;
        return l;
    } else if (l.kind == CONST_FLOAT) {
        l.float_val += r.float_val;
        return l;
    } else {
        return l;
    } 
}