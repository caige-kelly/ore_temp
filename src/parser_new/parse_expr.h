#ifndef ORE_PARSER_NEW_PARSE_EXPR_H
#define ORE_PARSER_NEW_PARSE_EXPR_H

#include "./parser.h"


// Precedence ladder — mirrors the old parser's Precedence enum so old
// reference code is readable side-by-side during the port. Documented in
// docs/GRAMMAR.md §2.3.
typedef enum {
    PREC_NONE = 0,
    PREC_BIND,         // ::  :=  :        (lowest operator; guarded LHS)
    PREC_LAMBDA,       // <-                 trailing-lambda / continuation
    PREC_ASSIGN,       // = += -= *= /= %= |= &= ~=    (right-assoc)
    PREC_OR,           // ||  orelse
    PREC_AND,          // &&
    PREC_EQUALITY,     // ==  !=
    PREC_COMPARISON,   // <  <=  >  >=
    PREC_BITWISE,      // |  &  ~
    PREC_SHIFT,        // <<  >>
    PREC_TERM,         // +  -
    PREC_FACTOR,       // *  /  %
    PREC_POWER,        // **                (right-assoc)
    PREC_UNARY,        // prefix - ! ~ & ^ ?  const
    PREC_POSTFIX,      // call, field, index, slice, ++ -- ^ ?
} Precedence;


// Parse an expression with the given minimum binding precedence.
// Pass PREC_NONE (0) to parse any expression. Emits one or more nodes
// into the green tree as a side effect.
void parse_expr(Parser *p, int precedence);

// Parse a type expression: `parse_expr` at PREC_BITWISE with the
// parsing_type flag set + restored. Reused from decl bind RHS, fn
// signatures, etc.
void parse_type_expr(Parser *p);

#endif // ORE_PARSER_NEW_PARSE_EXPR_H
