#ifndef ORE_PARSER_NEW_H
#define ORE_PARSER_NEW_H

// =====================================================================
// Green-tree-emitting parser (Phase A.1.2).
// =====================================================================
//
// Sole output is a `GreenNode *` rooted at SK_SOURCE_FILE. There is no
// flat-AST emission. Consumers in sema/db/ide migrate to read the green
// tree via the typed wrappers in [src/ast/](../ast/ast.h) and the
// navigation API in [src/syntax/](../syntax/syntax.h).
//
// DECOUPLED FROM db.h
// ===================
// The parser accumulates errors in a local `Vec<ParseError>` instead of
// calling `db_emit`. The caller (db/query/ast.c, post-A.1.3) drains the
// errors and translates them to diagnostics. This decoupling lets the
// parser be unit-tested via [tools/parser_green_test.c](../../tools/parser_green_test.c)
// without dragging the db layer into the test build.

#include <stdbool.h>
#include <stdint.h>

#include "../support/data_structure/vec.h"
#include "../syntax/syntax.h"
#include "../lexer/token.h"
#include "../syntax/syntax_kind.h"


// A parser-emitted error. Anchored at a token position so the caller can
// translate to a SyntaxNodePtr / byte range after the parse. The string
// is borrowed — error messages are string literals or arena-allocated.
typedef struct {
    uint32_t    tok_pos;   // token index in the input stream
    const char *msg;       // borrowed, must outlive the parse
} ParseError;


typedef struct {
    GreenBuilder *gb;          // output: the green tree under construction
    const Vec    *tokens;      // input: Vec<Token> — UNIFIED stream from
                                //        layout_stream (trivia + virtual
                                //        layout + real tokens, all in
                                //        document order). The cursor
                                //        primitives emit trivia to `gb`
                                //        and only stop at non-trivia
                                //        positions (see cursor invariant
                                //        in parser.c).
    const char   *source;      // input: the source-byte buffer; text is
                                //        passed to green_builder_token as
                                //        source + tok.start (length token_len)
    uint32_t      pos;         // cursor: current token index in `tokens`.
                                //        After any mutating cursor call,
                                //        points at a non-trivia token or
                                //        one-past-the-end.

    Vec           errors;      // Vec<ParseError>; drained by the caller

    // parsing_type: in type position (RHS of `:`, after `->`, inside
    // `Fn(...)`, `[N]`). Disables value-only forms (`[_]T{...}`,
    // `T{...}` initializer literals). Save/restore across recursion.
    bool          parsing_type;

    // in_distinct_rhs: parsing the RHS of a `distinct`-modified bind.
    // Disables the postfix `{` construction gate so `distinct u8 { … }`
    // parses the backing type then a PACKED BODY (bit-subfields), not
    // `u8`-construction.
    bool          in_distinct_rhs;
} Parser;


// ---------------------------------------------------------------------
// Public entry point.
// ---------------------------------------------------------------------
//
// Parses `tokens` (a Vec<Token> — the UNIFIED stream from
// layout_stream, including trivia, virtual layout tokens, and real
// tokens in document order) into a green tree rooted at SK_SOURCE_FILE.
// The resulting tree is lossless: green_node_text_len(root) ==
// total non-EOF byte count of the input.
//
// - `source`: the underlying source byte buffer; must outlive the parse.
//   Used as `source + tok.start` when emitting tokens to the builder.
// - `cache`: the workspace-level NodeCache; borrowed for the duration
//   of the build. Must outlive the returned GreenNode.
// - `out_errors`: filled with parse errors; caller takes ownership and
//   must vec_free when done. Initialize via `vec_init(out, sizeof(ParseError))`
//   before passing.
//
// Returns the root GreenNode, RETURNS_OWNED. Caller releases via
// green_node_release.
GreenNode *parse_file_green(const Vec *tokens, const char *source,
                             NodeCache *cache, Vec *out_errors);


// ---------------------------------------------------------------------
// Internal cursor primitives — used across parse_decl/stmt/expr.c.
// ---------------------------------------------------------------------

bool         p_is_eof(const Parser *p);
SyntaxKind   p_peek(const Parser *p);
SyntaxKind   p_peek_at(const Parser *p, uint32_t offset);
const Token *p_current(const Parser *p);

// Look ahead `offset` non-trivia tokens without advancing or emitting.
// p_token_at(p, 0) is equivalent to p_current(p). p_token_at(p, 1) is
// the next non-trivia token after pos. Returns NULL past EOF.
const Token *p_token_at(const Parser *p, uint32_t offset);

// Advance past the current token, emitting it to the green builder as a
// side effect (via green_builder_token). Returns the just-consumed token,
// or NULL at EOF (no advancement happens at EOF).
const Token *p_advance(Parser *p);

bool         p_match(Parser *p, SyntaxKind kind);

// If the current token matches `kind`, advances and returns it.
// Otherwise records an error and returns NULL.
//
// VIRTUAL-AWARE: when `kind` is SK_LBRACE/SK_RBRACE/SK_SEMI, this
// helper ALSO accepts the corresponding SK_VIRTUAL_* variant.
const Token *p_consume(Parser *p, SyntaxKind kind, const char *err_msg);

// Peek-style equivalent of p_match's predicate (virtual-aware).
bool         p_check(const Parser *p, SyntaxKind kind);

// Record a parser error at the current cursor position.
void         p_error(Parser *p, const char *msg);


// ---------------------------------------------------------------------
// Green-tree emission helpers.
//
// Thin wrappers around green_builder_* that thread the Parser as the
// implicit context. The parser code uses these everywhere; the raw
// builder API is only used inside parse_file_green.
// ---------------------------------------------------------------------

static inline void p_start_node(Parser *p, SyntaxKind kind) {
    green_builder_start_node(p->gb, kind);
}

static inline void p_finish_node(Parser *p) {
    green_builder_finish_node(p->gb);
}

static inline Checkpoint p_checkpoint(Parser *p) {
    return green_builder_checkpoint(p->gb);
}

// Retroactively wrap everything emitted since `cp` in a node of `kind`.
// The Pratt operator-collapse pattern: capture cp BEFORE the LHS, then
// start_node_at(cp, SK_BIN_EXPR) AFTER consuming the operator.
static inline void p_start_node_at(Parser *p, Checkpoint cp, SyntaxKind kind) {
    green_builder_start_node_at(p->gb, cp, kind);
}


#endif // ORE_PARSER_NEW_H
