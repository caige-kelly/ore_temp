#ifndef ORE_PARSER_NEW_PARSE_EXPR_H
#define ORE_PARSER_NEW_PARSE_EXPR_H

#include "./parser.h"


// Precedence ladder — mirrors the old parser's Precedence enum so old
// reference code is readable side-by-side during the port. Documented in
// docs/GRAMMAR.md §2.3.
typedef enum {
    PREC_NONE = 0,
    PREC_BIND,         // (statement-only; reserved for parse_stmt's bind RHS)
    PREC_LAMBDA,       // <-                 trailing-lambda / continuation
    PREC_ASSIGN,       // = += -= *= /= %= |= &= ~=    (right-assoc)
    PREC_OR,           // ||  orelse
    PREC_AND,          // &&
    PREC_EQUALITY,     // ==  !=
    PREC_COMPARISON,   // <  <=  >  >=
    PREC_RANGE,        // ..                 (range expression: lo..hi)
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

// Statement-only bind decls. parse_named_bind_decl handles `IDENT (::|:=|:)`
// at statement position; parse_bare_destructure_tail handles the bare
// `x, y (::|:=) value` destructure. These DO NOT fire inside parse_expr —
// variables are strictly statements.
void parse_named_bind_decl(Parser *p);
// Bare tuple-destructure `x, y (::|:=) value` (Slice 6.23). Precondition: the
// first target expr is already parsed at `lhs_cp` and the cursor sits on the
// first `,`. Wraps the comma-separated targets into SK_PRODUCT_EXPR and emits
// the SK_DESTRUCTURE_DECL (or a clean error if no `::`/`:=` follows).
void parse_bare_destructure_tail(Parser *p, Checkpoint lhs_cp);

// Single SK_PARAM (`[comptime] (NAME : TYPE | TYPE)`). Emits SK_PARAM
// into the green tree; the parent SK_PARAM_LIST is the caller's
// responsibility. `name_required = true` for op-clause / fn-decl / lambda
// params (name required, type optional); `false` for fn-type signatures
// like `fn(i32, bool)` (type only, no names).
void parse_param(Parser *p, bool name_required);

// `<` type [, type ...] [`...` | `..`name] `>` → SK_EFFECT_ROW_TYPE.
// Shared by fn-type effect annotations, mask, and the handler effect
// annotation.
void parse_effect_row(Parser *p);

#endif // ORE_PARSER_NEW_PARSE_EXPR_H
