#ifndef ORE_PARSE_EXPR_H
#define ORE_PARSE_EXPR_H

#include "./parser.h"

typedef enum {
    PREC_NONE = 0,
    PREC_ASSIGN,       // = += -= *= /= %= |= &= ^= <-          (right-assoc)
    PREC_OR,           // || orelse catch
    PREC_AND,          // &&
    PREC_EQUALITY,     // == !=
    PREC_COMPARISON,   // < <= > >=
    PREC_BITWISE,      // | & ^ ~
    PREC_SHIFT,        // << >>
    PREC_TERM,         // + -
    PREC_FACTOR,       // * / %
    PREC_POWER,        // **                                    (right-assoc)
    PREC_UNARY,        // prefix - ! ~ & * ^ ? const
    PREC_POSTFIX,      // call, field, index, slice, ++ ^ ? !
} Precedence;


// Parse an expression with a given minimum precedence level.
// Pass 0 to parse any expression.
AstNodeId parse_expr(Parser *p, int precedence);

#endif // ORE_PARSE_EXPR_H
