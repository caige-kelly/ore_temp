#ifndef ORE_PARSE_EXPR_H
#define ORE_PARSE_EXPR_H

#include "./parser.h"

// Parse an expression with a given minimum precedence level.
// Pass 0 to parse any expression.
AstNodeId parse_expr(Parser *p, int precedence);

#endif // ORE_PARSE_EXPR_H
