#ifndef ORE_LEXER_TOKEN_H
#define ORE_LEXER_TOKEN_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "../db/ids/ids.h"   // StrId

/*
    Token — the lexer's output unit.

    Layout (16 bytes, 4 per cache line):

        TokenKind kind;        // 1 B
        uint8_t   _pad0;       // 1 B (natural alignment of string_id)
        StrId     string_id;   // 4 B — interned text from StringPool;
                               //        STR_ID_NONE for tokens whose lexeme
                               //        is uninteresting (delimiters, ops)
        uint32_t  start;       // 4 B — inclusive start offset into source
        uint32_t  byte_end;    // 4 B — exclusive end offset

    Source text and line/column resolution:
    - byte_start/byte_end index into db.sources[file].text. FileId is
      implicit — the entire token vec belongs to one ModuleInfo (one
      file), so storing it per-token would be wasteful.
    - Line and column are NOT stored. Derive on demand from
      mod->line_starts via binary search at diagnostic / hover time. Most
      code paths only need byte offsets; line/col is the rare case.

    Synthetic vs source tokens:
    - The layout pass injects { } ; tokens that don't correspond to
      lexed text. These are emitted with start == byte_end == the
      previous source token's byte_end — a zero-width range that doubles
      as the "synthetic" marker. No separate origin flag needed.

    Contextual keywords:
    - `val`, `final`, `raw`, `ctl`, `override`, `named`, `in`,
      `scoped`, `linear` are NOT TokenKinds. They lex as TK_IDENTIFIER
      and the parser recognizes them by comparing tok.string_id against
      db.names.<kw> at positions where they could be meaningful.
    - Reserved keywords (everything below in the kind enum) ARE
      TokenKinds because they participate in expression-level dispatch
      and would break composability if recognized only locally.
*/

typedef enum : uint8_t {
    // ---- Special ---------------------------------------------------
    TK_EOF = 0,
    TK_ERROR,

    // ---- Trivia (stripped from the main stream by the layout pass;
    //               attached to mod->trivia_map keyed by following
    //               real-token index) ------------------------------
    TK_NEWLINE,
    TK_SPACE,
    TK_COMMENT,

    // ---- Literals --------------------------------------------------
    TK_IDENTIFIER,
    TK_INT_LIT,
    TK_FLOAT_LIT,
    TK_STRING_LIT,
    TK_BYTE_LIT,
    TK_ASM_LIT,

    // ---- Reserved keywords: values --------------------------------
    TK_TRUE,
    TK_FALSE,
    TK_NIL,
    TK_VOID,

    // ---- Reserved keywords: declarations --------------------------
    TK_FN,
    TK_FN_TYPE,         // capital `Fn` — function type
    TK_TYPE,
    TK_CONST,
    TK_STRUCT,
    TK_ENUM,
    TK_UNION,
    TK_EFFECT,
    TK_HANDLER,
    TK_PUB,
    TK_COMPTIME,
    TK_ANYTYPE,
    TK_NORETURN,

    // ---- Reserved keywords: control flow --------------------------
    TK_IF,
    TK_ELIF,
    TK_ELSE,
    TK_LOOP,
    TK_SWITCH,
    TK_BREAK,
    TK_CONTINUE,
    TK_RETURN,
    TK_DEFER,
    TK_ORELSE,

    // ---- Reserved keywords: effects / handlers --------------------
    TK_HANDLE,
    TK_MASK,
    TK_WITH,

    // (Contextual: val, final, raw, ctl, override, named, in,
    //              scoped, linear — lex as TK_IDENTIFIER.)

    // ---- Operators: logical ---------------------------------------
    TK_AMP_AMP,
    TK_PIPE_PIPE,
    TK_BANG,

    // ---- Operators: arithmetic ------------------------------------
    TK_PLUS,
    TK_MINUS,
    TK_STAR,
    TK_STAR_STAR,
    TK_SLASH,
    TK_PERCENT,

    // ---- Operators: bitwise ---------------------------------------
    TK_PIPE,
    TK_AMP,
    TK_CARET,
    TK_SHL,
    TK_SHR,

    // ---- Operators: relational ------------------------------------
    TK_EQ_EQ,
    TK_BANG_EQ,
    TK_LT,
    TK_LE,
    TK_GT,
    TK_GE,

    // ---- Operators: assignment ------------------------------------
    TK_EQ,
    TK_PLUS_EQ,
    TK_MINUS_EQ,
    TK_STAR_EQ,
    TK_SLASH_EQ,
    TK_PERCENT_EQ,
    TK_PIPE_EQ,
    TK_AMP_EQ,
    TK_CARET_EQ,
    TK_COLON_EQ,
    TK_PLUS_PLUS,

    // ---- Operators: other -----------------------------------------
    TK_RARROW,           // ->
    TK_LARROW,           // <-
    TK_FATARROW,         // =>
    TK_COLON,
    TK_COLON_COLON,
    TK_DOT,
    TK_DOT_DOT,
    TK_DOT_DOT_DOT,
    TK_QUESTION,
    TK_UNDERSCORE,

    // ---- Delimiters -----------------------------------------------
    TK_LPAREN,
    TK_RPAREN,
    TK_LBRACKET,
    TK_RBRACKET,
    TK_LBRACE,
    TK_RBRACE,
    TK_SEMI,
    TK_COMMA,
    TK_AT,
    TK_HASH,
    TK_TILDE,

    // Sentinel — count of distinct TokenKind values. Used to size
    // per-kind tables (token_kind_str dispatch, future telemetry).
    TK_COUNT,
} TokenKind;

// TK_COUNT fits in u8 with room to spare.
static_assert(TK_COUNT < 256, "TokenKind must fit in u8");


typedef struct {
    TokenKind kind;        // 1
    uint8_t   _pad0;       // 1
    StrId     string_id;   // 4 — STR_ID_NONE when the lexeme isn't worth interning
    uint32_t  start;       // 4
    uint32_t  byte_end;    // 4
} Token;

// Pin the wire size. Token is hot in lex/parse loops; 16 bytes packs
// exactly 4 per 64-byte cache line. Adding a field here is a perf
// regression — check first.
static_assert(sizeof(Token) == 16, "Token must stay at 16 bytes");


// Convenience predicates. Inlined to keep the parser's hot dispatch
// readable without paying for a call.

static inline bool token_is_trivia(TokenKind k) {
    return k == TK_NEWLINE || k == TK_SPACE || k == TK_COMMENT;
}

static inline bool token_is_synthetic(const Token *t) {
    // Layout-injected tokens have start == byte_end (zero-width
    // range, placed at the previous source token's end).
    return t->start == t->byte_end;
}

static inline uint32_t token_len(const Token *t) {
    return t->byte_end - t->start;
}


// Human-readable name for a TokenKind. Used by diagnostic messages and
// the `--dump-tokens` debug path. Implementation in token.c — single
// switch, no allocation.
const char *token_kind_str(TokenKind kind);

#endif // ORE_LEXER_TOKEN_H
