#ifndef CONST_EVAL_BIN_OPS_H
#define CONST_EVAL_BIN_OPS_H


#include "../const_eval.h"
#include "../../sema_internal.h"

struct ConstValue bin_add(struct Sema *s, struct Expr *expr, struct ConstValue l, struct ConstValue r);

#endif