#define _DEFAULT_SOURCE // Required for strndup
#include "./lexer.h"
#include "./token.h"
#include <ctype.h>
#include <string.h>

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
static struct Token make_token(struct Lexer* lexer, enum TokenKind kind) {
    struct Span span = span_new(lexer->start, lexer->current, lexer->line, lexer->start_column);
    // For string literals, we don't want to include the quotes in the lexeme.
    if (kind == StringLit || kind == ByteLit) {
        char* lexeme = strndup(&lexer->source[lexer->start + 1], lexer->current - lexer->start - 2);
        struct Token t = { kind, .lexeme = lexeme, .span = span, .origin = Source };
        return t;
    }
    char* lexeme = strndup(&lexer->source[lexer->start], lexer->current - lexer->start);
    struct Token t = { kind, .lexeme = lexeme, .span = span, .origin = Source };
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
static struct Token identifier_or_keyword(struct Lexer* lexer) {
    while (isalnum(lexer->source[lexer->current]) || lexer->source[lexer->current] == '_') {
        lexer->current++;
    }
    struct Token token = make_token(lexer, Identifier);
    token.kind = get_keyword_kind(token.lexeme);
    return token;
}

// Reads a number literal (integer or float).
static struct Token number(struct Lexer* lexer) {
    while (isdigit(lexer->source[lexer->current])) {
        lexer->current++;
    }
    if (lexer->source[lexer->current] == '.' && isdigit(lexer->source[lexer->current + 1])) {
        lexer->current++;
        while (isdigit(lexer->source[lexer->current])) {
            lexer->current++;
        }
        return make_token(lexer, FloatLit);
    }
    return make_token(lexer, IntLit);
}

// Reads a string literal.
static struct Token string(struct Lexer* lexer) {
    lexer->current++; // Consume the opening quote
    while (lexer->source[lexer->current] != '"' && lexer->source[lexer->current] != '\0') {
        if (lexer->source[lexer->current] == '\n') lexer->line++;
        // Handle escaped quotes
        if (lexer->source[lexer->current] == '\\' && lexer->source[lexer->current + 1] == '"') {
            lexer->current++;
        }
        lexer->current++;
    }

    if (lexer->source[lexer->current] == '\0') return make_token(lexer, Error); // Unterminated string.

    lexer->current++; // Consume the closing quote.
    return make_token(lexer, StringLit);
}

// Reads a byte literal.
static struct Token byte(struct Lexer* lexer) {
    lexer->current++; // Consume the opening quote
    while (lexer->source[lexer->current] != '\'' && lexer->source[lexer->current] != '\0') {
        if (lexer->source[lexer->current] == '\n') lexer->line++;
        lexer->current++;
    }

    if (lexer->source[lexer->current] == '\0') return make_token(lexer, Error); // Unterminated byte array.

    lexer->current++; // Consume the closing quote.
    return make_token(lexer, ByteLit);
}

// Helper to advance the lexer and return a token.
static struct Token advance_and_make_token(struct Lexer* lexer, enum TokenKind kind) {
    lexer->current++;
    return make_token(lexer, kind);
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

struct Token lexer_next_token(struct Lexer* lexer) {
    skip_whitespace_and_comments(lexer);
    lexer->start = lexer->current;

    char c = lexer->source[lexer->current];

    if (c == '\0') return make_token(lexer, Eof);

    if (isdigit(c)) return number(lexer);
    if (isalpha(c)) return identifier_or_keyword(lexer);

    switch (c) {
        case '"': return string(lexer);
        case '\'': return byte(lexer);
        case '(': return advance_and_make_token(lexer, LParen);
        return advance_and_make_token(lexer, LParen);
        case ')': return advance_and_make_token(lexer, RParen);
        case '[': return advance_and_make_token(lexer, LBracket);
        case ']': return advance_and_make_token(lexer, RBracket);
        case '{': return advance_and_make_token(lexer, LBrace);
        case '}': return advance_and_make_token(lexer, RBrace);
        case ';': return advance_and_make_token(lexer, Semicolon);
        case ',': return advance_and_make_token(lexer, Comma);
        case '@': return advance_and_make_token(lexer, At);
        case '#': return advance_and_make_token(lexer, Hash);
        case '~': return advance_and_make_token(lexer, Tilde);
        case '+': return advance_and_make_token(lexer, Plus);
        case '-': return advance_and_make_token(lexer, Minus);
        case '*': return advance_and_make_token(lexer, Star);
        case '/': return advance_and_make_token(lexer, ForwardSlash);
        case '%': return advance_and_make_token(lexer, Percent);
        case '&': return advance_and_make_token(lexer, Ampersand);
        case '|': return advance_and_make_token(lexer, Pipe);
        case '^': return advance_and_make_token(lexer, Caret);
        case '=': return advance_and_make_token(lexer, Equal);
        case '!': return advance_and_make_token(lexer, Bang);
        case '<': return advance_and_make_token(lexer, Less);
        case '>': return advance_and_make_token(lexer, Greater);
        case ':': return advance_and_make_token(lexer, Colon);
        case '.': return advance_and_make_token(lexer, Dot);
        case '?': return advance_and_make_token(lexer, Question);
        case '_': return advance_and_make_token(lexer, Underscore);
    }

    lexer->current++;
    return make_token(lexer, Error);
}
