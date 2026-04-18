#ifndef PARSER_H
#define PARSER_H

#include "../lexer/token.h"
#include "../common/vec.h"
#include "../common/stringpool.h"
#include "../common/arena.h"

struct Parser {
    Vec* tokens; // laid-out token stream
    size_t current; // current position
    StringPool* pool; // for looking up lexemes
    Arena* arena; // owns all AST nodes
};

// Initalize a parser
struct Parser parser_new(Vec* tokens, StringPool* pool);

// Parse the full token stream into a list
Vec* parse(struct Parser* p);

#endif // PARSER_H