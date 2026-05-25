#ifndef ORE_LEXER_LAYOUT_H
#define ORE_LEXER_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

#include "./token.h"
#include "./lexer.h"
#include "../support/data_structure/vec.h"

// Trivia is stored as a zero-copy source range, NOT a copied Token.
// 8 bytes vs a 16-byte Token — halves the trivia store and removes the
// per-trivia-token memmove in the layout pass. The kind (SPACE /
// NEWLINE / COMMENT) is re-derivable from source bytes at `start` by
// the (out-of-build) formatter/hover consumer; widen with a `u8 kind`
// field only if a real consumer needs it without re-scanning source.
typedef struct {
    uint32_t start;  // inclusive byte offset into the module's source
    uint32_t end;    // exclusive byte offset
} TriviaSpan;

/*
    Layout pass — single-pass Koka-style normalization, FUSED with the
    lexer. Instead of consuming a pre-built Vec<Token>, the driver pulls
    one raw token at a time from a LexCursor (lex_next); there is no
    intermediate raw-token array. The real+synthetic stream is written
    directly to out_real_tokens.

    Input:  LexCursor (lex_begin'd) — yields TK_SPACE / TK_NEWLINE /
            TK_COMMENT trivia interleaved with real tokens, then TK_EOF.
            `line_starts` is the cursor's own line-start Vec, grown live
            as it scans; it is always complete up to the line containing
            the most recently pulled token (all get_column needs).

    Outputs (three caller-supplied Vecs):
      out_real_tokens     : Vec<Token> — no trivia, plus synthetic
                            TK_LBRACE/TK_RBRACE/TK_SEMI injected per
                            Koka-style indent rules.
      out_trivia_tokens   : Vec<TriviaSpan> — every trivia token as an
                            8-byte source range, in order (zero-copy:
                            no Token is materialized).
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

    Memory: no internal allocation. Output Vecs are caller-init'd
    (malloc-backed vec_init); the caller may vec_reserve to a capacity
    hint to avoid doubling reallocs. vec_clear'd on entry.
*/
void layout_stream(LexCursor *lc,
                   const Vec *line_starts,
                   Vec       *out_real_tokens,
                   Vec       *out_trivia_tokens,
                   Vec       *out_trivia_offsets);

#endif // ORE_LEXER_LAYOUT_H
