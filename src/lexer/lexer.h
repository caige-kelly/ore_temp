#ifndef LEXER_H
#define LEXER_H

#include <stddef.h>

#include "./token.h"
#include "common/stringpool.h"

// A struct defining a lexical error
struct LexError {
    const char* message;
    size_t line;
    size_t column;
};

// The Lexer struct
struct Lexer {
    size_t start;
    size_t current;
    size_t line;
    size_t column;
    size_t start_line;
    size_t start_column;
    bool at_line_start;
    int file_id;
    const char* source;
};

// ------------
// Function Prototypes
// ------------

struct Lexer lexer_new(const char* source, int file_id);
struct Token tokenizer(struct Lexer* lexer, StringPool* pool);

#endif // LEXER_H
