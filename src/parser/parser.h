#ifndef PARSER_H
#define PARSER_H

#include "../lexer/token.h"
#include "../common/vec.h"
#include "../common/stringpool.h"
#include "../common/arena.h"
#include "../diag/diag.h"
#include "./ast.h"


struct Parser {
    Vec* tokens; // laid-out token stream
    size_t current; // current position
    StringPool* pool; // for looking up lexemes
    Arena* arena; // owns all AST nodes
    struct DiagBag* diags; // optional parser diagnostics sink
    bool had_error;
    bool parsing_type; // whether we're parsing a type
    // > 0 while parsing the body of a `handler { ... }` or `handle (t) { ... }`
    // — gates `initially`/`finally` keywords from appearing at the head of
    // a stmt outside that context.
    int in_handler_block_depth;
    // True when trailing-lambda postfix (`f { block }` and `f fn(...) body`)
    // is allowed in the current expression context. Disabled inside
    // contexts that do their own body consumption (e.g., `with caller body`)
    // to avoid double-consuming the body block.
    bool allow_trailing_lam;
    // Pre-interned string IDs for keywords/names looked up on the hot path.
    // Set once in parser_new_in_with_diags so the parser doesn't
    // re-intern these every time it inspects a Bind name in a handler
    // block or checks a parameter type for "Scope".
    struct {
        uint32_t initially;
        uint32_t finally;
        uint32_t scope;
        uint32_t behind;
    } interned;
    // Monotonically increasing NodeId counter, assigned to every AST node
    // by alloc_expr. Starts at 1 so `(struct NodeId){0}` reads as "unset"
    // for any synthesized/placeholder node downstream passes might
    // produce.
    uint32_t next_node_id;
};

// Initialize a parser. The compiler always supplies an arena and diag bag;
// the no-arena and no-diags overloads were dead and the no-arena one
// leaked the Arena it allocated. Removed.
struct Parser parser_new_in_with_diags(Vec* tokens, StringPool* pool, Arena* arena,
                                       struct DiagBag* diags);

// Parse the full token stream into a list
Vec* parse(struct Parser* p);

void print_ast(struct Expr* expr, StringPool* pool, int indent);


#endif // PARSER_H