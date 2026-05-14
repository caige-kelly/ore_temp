#ifndef ORE_LEXER_H
#define ORE_LEXER_H

#include <stdbool.h>
#include <stdint.h>

#include "./token.h"
#include "../db/storage/stringpool.h"
#include "../db/storage/vec.h"

/*
    Lexer entry point.

    Pure function over source bytes:
        source bytes → (tokens + line_starts)

    No db dependency. Lex errors are encoded as TK_ERROR tokens whose
    byte range covers the offending region; intended error messages are
    preserved as comments next to each `lex_error` call site in
    lexer.c. When the diag subsystem is reachable from here (i.e.,
    when this gets wrapped by a query body with `struct db *` access),
    swap the no-op `lex_error` stub for `db_diag_error_*` calls.

    Emitted token stream:
    - Real tokens: identifiers, keywords, literals, operators, delimiters
    - Trivia tokens: TK_SPACE, TK_NEWLINE, TK_COMMENT — preserved through
      lex so the layout pass can attach them to mod->trivia_map and
      strip them from the real stream
    - TK_ERROR: byte range covers the offending region
    - TK_EOF: terminates the stream (one final token at end of source)

    Memory ownership (no internal allocation):
    - `out_tokens` MUST be caller-init'd via `vec_init_in_arena` against
      a scratch arena (typically `db.request_arena`), sized to
      `source_len` as a conservative upper bound. Avoids reallocs on
      the hot path; reclaimed when the calling query body resets the
      scratch arena via `arena_reset_to`.
    - `out_line_starts` MUST be caller-init'd via `vec_init_in_arena`
      against the per-module arena (`mod->arena`), so line/col
      derivation survives past lex/parse scratch. Conservative upper
      bound: `source_len/4` (dense code averages ~32 bytes/line).
    - `pool` is borrowed for the duration of the call. Lexemes worth
      interning (identifiers, literal text, asm bodies) get a real
      StrId; uninteresting lexemes (operators, delimiters, EOF) carry
      STR_ID_NONE.

    Reserved-keyword recognition is internal — a small static table of
    ~30 reserved keywords. Contextual keywords (`val`, `final`, `raw`,
    `ctl`, `override`, `named`, `in`, `scoped`, `linear`) are NOT in
    the table — they lex as TK_IDENTIFIER and the parser disambiguates
    via `tok.string_id` compared against `db.names.<kw>` at positions
    where they could appear.

    Source contract: `source[source_len]` must be readable and equal to
    `'\0'`. `db_alloc_source` guarantees this.
*/
void lex(const char *source,
         uint32_t    source_len,
         StringPool *pool,
         Vec        *out_tokens,
         Vec        *out_line_starts);

#endif // ORE_LEXER_H
