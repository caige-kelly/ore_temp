#ifndef ORE_LEXER_LAYOUT_H
#define ORE_LEXER_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

#include "./token.h"
#include "../db/storage/vec.h"

/*
    Layout pass — single-pass normalization of the raw token stream.

    Input:  Vec<Token> from lex() — includes TK_SPACE, TK_NEWLINE,
            TK_COMMENT interleaved with real tokens.

    Outputs (three caller-supplied, arena-backed Vecs):
      out_real_tokens     : Vec<Token> — no trivia, plus synthetic
                            TK_LBRACE/TK_RBRACE/TK_SEMI injected per
                            Koka-style indent rules.
      out_trivia_tokens   : Vec<Token> — every trivia token from input
                            in source order.
      out_trivia_offsets  : Vec<uint32_t> — parallel to out_real_tokens,
                            plus one trailing sentinel. The trivia
                            preceding real-token `i` is
                              trivia_tokens[offsets[i] .. offsets[i+1]]

    Synthetic layout tokens have byte_start == byte_end == previous
    source-token's byte_end (zero-width range). They get an empty
    trivia slot (offsets[i] == offsets[i+1]).

    Layout rules:
    - Koka-style: a line indented MORE than the enclosing layout column
      opens a new implicit block (synthetic `{`); a line indented LESS
      closes blocks (synthetic `;` + `}`); a line at the same column is
      a sibling of the previous statement (synthetic `;`).
    - Continuation predicates (`is_expr_continuation`) suppress the
      semicolon/brace insertion when the line is clearly continuing an
      expression (trailing binary op, dangling `(`/`[`/`,`, etc.).
    - Explicit `{` ... `}` opens an Explicit frame: indentation rules
      are bypassed inside; the closing `}` must be explicit.

    Error handling:
    - Lex errors flow through as TK_ERROR tokens, pass-through unchanged.
    - Layout-level "soft" errors (comment in indentation, dedent past an
      explicit `{`, unmatched `}`) are recognized in the code but their
      diag emission is currently a no-op stub. Re-enable when layout
      runs with `struct db *` access (i.e., from QUERY_MODULE_AST's body
      after the parser is wired). See `layout_error` in layout.c.

    Memory:
    - No internal allocation. All output Vecs are caller-init'd via
      vec_init_in_arena against the appropriate arenas:
        out_real_tokens     → request_arena (scratch; parser consumes)
        out_trivia_tokens   → mod->arena   (durable; hover walks)
        out_trivia_offsets  → mod->arena   (durable)
    - Conservative caps (caller-side):
        out_real_tokens    : in_raw_tokens->count * 2  (synthetic injections)
        out_trivia_tokens  : in_raw_tokens->count       (trivia ⊆ input)
        out_trivia_offsets : in_raw_tokens->count + 1   (one per real + sentinel)
*/
void layout(const Vec      *in_raw_tokens,
            const uint32_t *line_starts,
            size_t          n_line_starts,
            Vec            *out_real_tokens,
            Vec            *out_trivia_tokens,
            Vec            *out_trivia_offsets);

#endif // ORE_LEXER_LAYOUT_H
