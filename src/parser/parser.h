#ifndef PARSER_H
#define PARSER_H

#include "../lexer/token.h"
#include "../db/storage/vec.h"
#include "../db/storage/stringpool.h"
#include "../db/storage/arena.h"
#include "../support/diag.h"
#include "./ast.h"


struct Parser {
    // --- Inputs ---
    Vec* tokens;              // laid-out token stream
    size_t current;           // current position
    StringPool* pool;         // for looking up lexemes
    
    // --- Outputs (The Database) ---
    ASTStore* ast;            // The parser only pushes to this.
    
    // > 0 while parsing the body of a `handler { ... }` or `handle (t) { ... }`
    int in_handler_block_depth;
    
    // True when trailing-lambda postfix (`f { block }`) is allowed
    bool allow_trailing_lam;

    // Pre-interned string IDs for keywords/names on the hot path.
    struct {
        StrId initially;
        StrId finally;
        StrId scope;
        StrId behind;
    } interned;
};

// Parse the full token stream into a list
void parse_file(struct Parser* p, const char* source, Arena* scratch_arena);

void print_ast(struct Expr* expr, StringPool* pool, int indent);

#endif // PARSER_H