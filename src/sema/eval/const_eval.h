#ifndef SEMA_EVAL_CONST_EVAL_H
#define SEMA_EVAL_CONST_EVAL_H

#include <stdint.h>
#include <stdbool.h>

struct Sema;
struct Expr;

typedef enum {
    CONST_NONE,
    CONST_INT,
    CONST_FLOAT

} ConstKind;

struct ConstValue {
    ConstKind kind;
    union {
        int64_t int_val;
        double float_val;
    };
};

struct ConstValue query_const_eval(struct Sema *s, struct Expr *expr);

#endif