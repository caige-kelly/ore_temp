#ifndef ORE_LEXER_TOKEN_H
#define ORE_LEXER_TOKEN_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "../db/ids/ids.h"          // StrId
#include "../syntax/syntax_kind.h"  // SyntaxKind (= uint16_t) + SK_* constants

/*
    Token — the lexer's output unit.

    Layout (16 bytes, 4 per cache line):

        SyntaxKind kind;       // 2 B — uint16_t SyntaxKind from
                               //       src/syntax/syntax.h. Passes directly
                               //       to green_builder_token in Phase A.1.
        uint16_t   _pad0;      // 2 B — natural alignment of string_id
        StrId      string_id;  // 4 B — interned text from StringPool;
                               //        STR_ID_NONE for tokens whose lexeme
                               //        is uninteresting (delimiters, ops)
        uint32_t   start;      // 4 B — inclusive start offset into source
        uint32_t   byte_end;   // 4 B — exclusive end offset

    Source text and line/column resolution:
    - start/byte_end index into the file's source text. FileId is
      implicit — the entire token vec belongs to one file, so storing it
      per-token would be wasteful.
    - Line and column are NOT stored. Derive on demand from the file's
      line index (QUERY_LINE_INDEX → files.arenas[fid]) via binary search
      at diagnostic / hover time.

    Synthetic vs source tokens:
    - The layout pass injects { } ; tokens that don't correspond to
      lexed text. Through Phase A.0.2, these reuse SK_LBRACE / SK_RBRACE
      / SK_SEMI with start == byte_end (zero-width range) as the sole
      discriminator. Phase A.0.3 switches them to dedicated
      SK_VIRTUAL_LBRACE / SK_VIRTUAL_RBRACE / SK_VIRTUAL_SEMI kinds.

    Contextual keywords:
    - `val`, `final`, `raw`, `ctl`, `override`, `named`, `in`,
      `scoped`, `linear` are NOT kinds. They lex as SK_IDENT and the
      parser recognizes them by comparing the token's SOURCE BYTES
      against the keyword literal (tok_str_eq / TOK_IS in parse_expr.c)
      at positions where they could be meaningful.
    - Reserved keywords (the SK_*_KW values from syntax_kind.h) ARE
      first-class kinds because they participate in expression-level
      dispatch.
*/

typedef struct {
    SyntaxKind kind;       // 2
    uint16_t   _pad0;      // 2
    StrId      string_id;  // 4 — STR_ID_NONE when the lexeme isn't worth interning
    uint32_t   start;      // 4
    uint32_t   byte_end;   // 4
} Token;

// Pin the wire size. Token is hot in lex/parse loops; 16 bytes packs
// exactly 4 per 64-byte cache line. Adding a field here is a perf
// regression — check first.
static_assert(sizeof(Token) == 16, "Token must stay at 16 bytes");


// Convenience predicates. Inlined to keep the parser's hot dispatch
// readable without paying for a call.

static inline bool token_is_trivia(SyntaxKind k) {
    return ore_kind_is_trivia((OreSyntaxKind)k);
}

static inline bool token_is_synthetic(const Token *t) {
    // Kind is the single source of truth for synthetic-ness:
    // SK_VIRTUAL_LBRACE / SK_VIRTUAL_RBRACE / SK_VIRTUAL_SEMI are the
    // only synthetic kinds layout.c emits. They all have zero-width
    // range too (start == byte_end), but the kind is the authoritative
    // check — won't get fooled by a hypothetical future zero-width
    // real token.
    return ore_kind_is_virtual_layout((OreSyntaxKind)t->kind);
}

static inline uint32_t token_len(const Token *t) {
    return t->byte_end - t->start;
}


// Human-readable name for a token kind. Used by diagnostic messages
// and the `--dump-tokens` debug path. Returns the SOURCE-form name
// (e.g., "==", "+=", "fn") — different from ore_syntax_kind_name(),
// which returns the enum name (e.g., "EQ_EQ", "FN_KW"). Implementation
// in token.c — single switch, no allocation.
const char *token_kind_str(SyntaxKind kind);

#endif // ORE_LEXER_TOKEN_H
