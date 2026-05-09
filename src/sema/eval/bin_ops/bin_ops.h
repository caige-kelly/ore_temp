#ifndef CONST_EVAL_BIN_OPS_H
#define CONST_EVAL_BIN_OPS_H

#include "../../sema.h"
#include "../../../parser/ast.h"

struct ConstValue bin_add(struct Sema *s, struct Expr *expr, struct ConstValue l, struct ConstValue r);
struct ConstValue bin_sub(struct Sema *s, struct Expr *expr, struct ConstValue l, struct ConstValue r);
struct ConstValue bin_mul(struct Sema *s, struct Expr *expr, struct ConstValue l, struct ConstValue r);
struct ConstValue bin_div(struct Sema *s, struct Expr *expr, struct ConstValue l, struct ConstValue r);
struct ConstValue bin_mod(struct Sema *s, struct Expr *expr, struct ConstValue l, struct ConstValue r);

#endif