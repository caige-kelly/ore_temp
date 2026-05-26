#ifndef ORE_LEXER_LAYOUT_H
#define ORE_LEXER_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

#include "./lexer.h"
#include "./token.h"
#include "../support/data_structure/vec.h"

/*
    Layout pass — single-pass Koka-style normalization, FUSED with the
    lexer. Pulls one raw token at a time from a LexCursor (lex_next);
    there is no intermediate raw-token array.

    Input:  LexCursor (lex_begin'd) — yields SK_WHITESPACE / SK_NEWLINE /
            SK_COMMENT trivia interleaved with real tokens, then SK_EOF.
            `line_starts` is the cursor's own line-start Vec, grown live
            as it scans; it is always complete up to the line containing
            the most recently pulled token (all get_column needs).

    Output:  one Vec<Token> in document order. Every byte is represented:
      - Trivia tokens (SK_WHITESPACE / SK_NEWLINE / SK_COMMENT) flow
        through in the position the lexer emitted them.
      - Real tokens flow through unchanged.
      - Synthetic layout tokens (SK_VIRTUAL_LBRACE / SK_VIRTUAL_RBRACE /
        SK_VIRTUAL_SEMI) are inserted with start == byte_end == previous
        real token's byte_end (zero-width). They sit BEFORE any trivia
        between the previous real token and the next, so the stream's
        token order is positional in source.

    Consumers can distinguish each token via:
      - ore_kind_is_trivia(t.kind)         — whitespace/newline/comment
      - ore_kind_is_virtual_layout(t.kind) — synthetic braces/semis
      - everything else                    — real lexer-emitted tokens

    The green-tree builder consumes this directly:
      green_builder_token(b, t.kind, source + t.start, token_len(&t))
    for every t in order.

    Layout rules (unchanged from the prior 3-Vec version):
    - Koka-style: a line indented MORE than the enclosing layout column
      opens a new implicit block (SK_VIRTUAL_LBRACE); a line indented
      LESS closes blocks (SK_VIRTUAL_SEMI + SK_VIRTUAL_RBRACE); a line
      at the same column is a sibling of the previous statement
      (SK_VIRTUAL_SEMI).
    - Continuation predicates (`is_expr_continuation`) suppress the
      semicolon/brace insertion when the line is clearly continuing an
      expression (trailing binary op, dangling `(`/`[`/`,`, etc.).
    - Explicit `{` ... `}` opens an Explicit frame: indentation rules
      are bypassed inside; the closing `}` must be explicit.

    Error handling:
    - Lex errors flow through as SK_LEX_ERROR tokens, pass-through unchanged.

    Memory: no internal heap allocation beyond a tiny stack scratch
    buffer for pending trivia (sized at LAYOUT_TRIVIA_SCRATCH; asserts
    on overflow). The output Vec is caller-init'd (malloc-backed
    vec_init); the caller may vec_reserve to a capacity hint to avoid
    doubling reallocs. vec_clear'd on entry.
*/
void layout_stream(LexCursor *lc,
                   const Vec *line_starts,
                   Vec       *out_tokens);

#endif  // ORE_LEXER_LAYOUT_H
