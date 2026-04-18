#include "./lexer.h"
#include "./token.h"
#include <ctype.h>
#include <string.h>
#include <stddef.h>
#include "../common/stringpool.h"


struct Lexer lexer_new(const char* source, int file_id) {
    struct Lexer l = {
        .source = source,
        .start = 0,
        .current = 0,
        .line = 1,
        .column = 1,
        .start_column = 1,
        .file_id  = file_id,
        .at_line_start = true,
    };
    return l;
}

// Helper to create a token.
static struct Token make_token(struct Lexer* lexer, StringPool* pool, enum TokenKind kind) {
    struct Span span = span_new(lexer->start, lexer->current, lexer->line, lexer->start_column);

    size_t len = lexer->current - lexer->start;
    const char* src = &lexer->source[lexer->start];
    // For string literals, we don't want to include the quotes in the lexeme.
    if (kind == StringLit || kind == ByteLit) {
        char* lexeme = strndup(&lexer->source[lexer->start + 1], lexer->current - lexer->start - 2);
        struct Token t = { kind, .string_id = pool_intern(pool, lexeme), .string_len = len - 2, .span = span, .origin = Source };
        return t;
    }
    char* lexeme = strndup(&lexer->source[lexer->start], lexer->current - lexer->start);
    struct Token t = { kind, .string_id = pool_intern(pool, lexeme), .string_len = len, .span = span, .origin = Source };
    return t;
}

// Helper to get the token kind for a given keyword string.
static enum TokenKind get_keyword_kind(const char* keyword) {
    if (strcmp(keyword, "if") == 0) return If;
    if (strcmp(keyword, "then") == 0) return Then;
    if (strcmp(keyword, "else") == 0) return Else;
    if (strcmp(keyword, "return") == 0) return Return;
    if (strcmp(keyword, "for") == 0) return For;
    if (strcmp(keyword, "true") == 0) return True;
    if (strcmp(keyword, "false") == 0) return False;
    if (strcmp(keyword, "nil") == 0) return Nil;
    return Identifier;
}

// Reads a full identifier or keyword.
static struct Token identifier_or_keyword(struct Lexer* lexer, StringPool* pool) {
    while (isalnum(lexer->source[lexer->current]) || lexer->source[lexer->current] == '_') {
        lexer->current++;
    }
    struct Token token = make_token(lexer, pool, Identifier);
    token.kind = get_keyword_kind(pool_get(pool, (void*)token.string_id));
    return token;
}

// Reads a number literal (integer or float).
static struct Token number(struct Lexer* lexer, StringPool* pool) {
    while (isdigit(lexer->source[lexer->current])) {
        lexer->current++;
    }
    if (lexer->source[lexer->current] == '.' && isdigit(lexer->source[lexer->current + 1])) {
        lexer->current++;
        while (isdigit(lexer->source[lexer->current])) {
            lexer->current++;
        }
        return make_token(lexer,pool, FloatLit);
    }
    return make_token(lexer,pool, IntLit);
}

// Reads a string literal.
static struct Token string(struct Lexer* lexer, StringPool* pool) {
    lexer->current++; // Consume the opening quote
    while (lexer->source[lexer->current] != '"' && lexer->source[lexer->current] != '\0') {
        if (lexer->source[lexer->current] == '\n') lexer->line++;
        // Handle escaped quotes
        if (lexer->source[lexer->current] == '\\' && lexer->source[lexer->current + 1] == '"') {
            lexer->current++;
        }
        lexer->current++;
    }

    if (lexer->source[lexer->current] == '\0') return make_token(lexer,pool, Error); // Unterminated string.

    lexer->current++; // Consume the closing quote.
    return make_token(lexer,pool, StringLit);
}

// Reads a byte literal.
static struct Token byte(struct Lexer* lexer, StringPool* pool) {
    lexer->current++; // Consume the opening quote
    while (lexer->source[lexer->current] != '\'' && lexer->source[lexer->current] != '\0') {
        if (lexer->source[lexer->current] == '\n') lexer->line++;
        lexer->current++;
    }

    if (lexer->source[lexer->current] == '\0') return make_token(lexer,pool, Error); // Unterminated byte array.

    lexer->current++; // Consume the closing quote.
    return make_token(lexer,pool, ByteLit);
}

// Helper to advance the lexer and return a token.
static struct Token advance_and_make_token(struct Lexer* lexer, StringPool* pool, enum TokenKind kind) {
    lexer->current++;
    return make_token(lexer, pool, kind);
}

// Skips whitespace and comments.
static void skip_whitespace_and_comments(struct Lexer* lexer) {
    for (;;) {
        char c = lexer->source[lexer->current];
        switch (c) {
            case ' ': case '\r': case '\t': lexer->current++; break;
            case '\n': lexer->line++; lexer->current++; break;
            case '/':
                if (lexer->source[lexer->current + 1] == '/') {
                    while (lexer->source[lexer->current] != '\n' && lexer->source[lexer->current] != '\0') {
                        lexer->current++;
                    }
                } else {
                    return;
                }
                break;
            default: return;
        }
    }
}

struct Token tokenizer(struct Lexer* lexer, StringPool* pool) {
    skip_whitespace_and_comments(lexer);
    lexer->start = lexer->current;

    char c = lexer->source[lexer->current];

    if (c == '\0') return make_token(lexer,pool, Eof);

    if (isdigit(c)) return number(lexer, pool);
    if (isalpha(c)) return identifier_or_keyword(lexer, pool);

    switch (c) {
        case '"': return string(lexer, pool);
        case '\'': return byte(lexer, pool);
        case '(': return advance_and_make_token(lexer,pool,LParen);
        return advance_and_make_token(lexer,pool,LParen);
        case ')': return advance_and_make_token(lexer,pool,RParen);
        case '[': return advance_and_make_token(lexer,pool,LBracket);
        case ']': return advance_and_make_token(lexer,pool,RBracket);
        case '{': return advance_and_make_token(lexer,pool,LBrace);
        case '}': return advance_and_make_token(lexer,pool,RBrace);
        case ';': return advance_and_make_token(lexer,pool,Semicolon);
        case ',': return advance_and_make_token(lexer,pool,Comma);
        case '@': return advance_and_make_token(lexer,pool,At);
        case '#': return advance_and_make_token(lexer,pool,Hash);
        case '~': return advance_and_make_token(lexer,pool,Tilde);
        case '+': return advance_and_make_token(lexer,pool,Plus);
        case '-': return advance_and_make_token(lexer,pool,Minus);
        case '*': return advance_and_make_token(lexer,pool,Star);
        case '/': return advance_and_make_token(lexer,pool,ForwardSlash);
        case '%': return advance_and_make_token(lexer,pool,Percent);
        case '&': return advance_and_make_token(lexer,pool,Ampersand);
        case '|': return advance_and_make_token(lexer,pool,Pipe);
        case '^': return advance_and_make_token(lexer,pool,Caret);
        case '=': return advance_and_make_token(lexer,pool,Equal);
        case '!': return advance_and_make_token(lexer,pool,Bang);
        case '<': return advance_and_make_token(lexer,pool,Less);
        case '>': return advance_and_make_token(lexer,pool,Greater);
        case ':': return advance_and_make_token(lexer,pool,Colon);
        case '.': return advance_and_make_token(lexer,pool,Dot);
        case '?': return advance_and_make_token(lexer,pool,Question);
        case '_': return advance_and_make_token(lexer,pool,Underscore);
    }

    lexer->current++;
    return make_token(lexer,pool, Error);
}
