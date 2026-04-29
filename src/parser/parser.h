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
};

// Initalize a parser
struct Parser parser_new(Vec* tokens, StringPool* pool);
struct Parser parser_new_in(Vec* tokens, StringPool* pool, Arena* arena);
struct Parser parser_new_in_with_diags(Vec* tokens, StringPool* pool, Arena* arena,
                                       struct DiagBag* diags);

// Parse the full token stream into a list
Vec* parse(struct Parser* p);

void print_ast(struct Expr* expr, StringPool* pool, int indent);


#endif // PARSER_H