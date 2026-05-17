#ifndef ORE_PARSE_EXPR_H
#define ORE_PARSE_EXPR_H

#include "./parser.h"

typedef enum {
    PREC_NONE = 0,
    PREC_BIND,         // :: := :  (lowest operator; guarded — LHS must be a name/pattern)
    PREC_LAMBDA,       // <-  trailing-lambda / continuation (just above BIND
                       // so a bind RHS consumes it; below switch's PREC_OR+1
                       // pattern floor so `=>` arm-separators stay isolated)
    PREC_ASSIGN,       // = += -= *= /= %= |= &= ^=             (right-assoc)
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

// Parse a type expression: `parse_expr` at PREC_BITWISE with the
// parsing_type flag set/restored. Shared with parse_decl's typed binds.
AstNodeId parse_type_expr(Parser *p);

#endif // ORE_PARSE_EXPR_H
