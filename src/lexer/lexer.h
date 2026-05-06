#ifndef LEXER_H
#define LEXER_H

#include <stdbool.h>
#include <stddef.h>

#include "../diag/diag.h"
#include "./token.h"
#include "common/stringpool.h"

// The lexer scans a source buffer one token at a time. It owns no memory
// (source is borrowed; tokens are interned via the StringPool the caller
// passes in). Diagnostics are pushed to the borrowed DiagBag — when the
// lexer can't classify a character it emits an `Error` token AND a diag,
// then advances; the caller treats Error as a recovery point.
struct Lexer {
    const char* source;        // borrowed, NUL-terminated
    size_t source_len;         // length excluding the trailing NUL
    size_t start;              // byte offset where the in-progress token began
    size_t current;            // current byte offset
    size_t line;               // current line (1-based)
    size_t column;             // current column (1-based)
    size_t start_line;         // line where the in-progress token began
    size_t start_column;       // column where the in-progress token began
    int file_id;
    struct DiagBag* diags;     // borrowed; may be NULL for tooling/tests
};

// ------------
// Function Prototypes
// ------------

struct Lexer lexer_new(const char* source, int file_id, struct DiagBag* diags);
struct Token tokenizer(struct Lexer* lexer, StringPool* pool);

#endif // LEXER_H
