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

#include "../db/ids/ids.h"                       // StrId
#include "../support/data_structure/stringpool.h" // StringPool, pool_intern
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


// Pre-interned StrIds for every contextual keyword the parser
// recognizes. Populated once at the top of `parse_file_green` so all
// "is this token the literal X?" checks are a single u32 compare
// against `t->string_id` — never a memcmp against `p->source`.
//
// Naming: trailing underscore where the bare word collides with C
// reserved or commonly-confusing identifiers (`final_`, `abstract_`,
// `distinct_`, `in_`).
typedef struct {
    // Decl modifiers (`named effect` / `scoped effect` — handled by
    // at_modifier_kw in parse_expr.c). `override` is gone: it was a
    // handler modifier (a deferred `mask`-desugar), never a decl one.
    StrId named, scoped;

    // Visibility.
    StrId pub, pvt;

    // Decl-flavor markers.
    StrId abstract_, distinct_, linear;

    // Handler op-clause flavors. Koka also accepts `control` (deprecated
    // alias of `ctl`), `except` / `brk` (deprecated `final ctl`), and
    // `rcontrol` / `rawctl` (deprecated `raw ctl`); ore does not — no
    // legacy code to migrate.
    //
    // `raw ctl` itself is also dropped: it suppresses Koka's automatic
    // `Finalize` injection so the user can reify the continuation as a
    // first-class value, escape lexical scope, multi-shot resume, and
    // hand-roll finalization timing. Ore compiles every `ctl` to a
    // single-shot state machine — none of those capabilities exist by
    // design, so there's nothing for an `OpControlRaw` sort to mean.
    //
    // Bind-style (Slice 6.16): `ctl` and `final-ctl` are RHS lambda-
    // introducers (`name :: ctl(params) body`). `final-ctl` is a single
    // kebab token (the lexer joins `final-ctl` into one ident). `final_`
    // (the two-word `final ctl`) is transitional — removed in Step 4.
    StrId ctl, val, final_, final_ctl;

    // Handler lifecycle clauses are dropped under single-shot semantics:
    //   - `initially` re-runs on every continuation resumption (its
    //     `rcount` arg is the resume count) — only matters for multi-
    //     shot; single-shot setup runs once, so pre-`with` locals are
    //     identical and simpler.
    //   - `finally` is handler-attached cleanup that only earns its keep
    //     when a handler value escapes its `with` frame. Ore's scope-safe
    //     single-shot handlers can't outlive their frame, so a `defer` in
    //     the surrounding fn covers every realistic cleanup.
    // (No fields — neither keyword is recognized.)

    // Mask construct.
    StrId behind;

    // Loop / similar binder.
    StrId in_;
} ParserKws;


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
    StringPool   *pool;        // borrowed: the same pool the lexer interned
                                //        into. Used to pre-intern the
                                //        contextual-keyword table once at
                                //        parser init.
    ParserKws     kws;         // pre-interned StrIds for fast contextual-
                                //        keyword recognition (see ParserKws).
    uint32_t      pos;         // cursor: current token index in `tokens`.
                                //        After any mutating cursor call,
                                //        points at a non-trivia token or
                                //        one-past-the-end.

    Vec           errors;      // Vec<ParseError>; drained by the caller

    // parsing_type: in type position (RHS of `:`, after `->`, inside
    // `Fn(...)`, `[N]`). Disables value-only forms (`[_]T{...}`,
    // `T{...}` initializer literals). Save/restore across recursion.
    bool          parsing_type;

    // fix_count: total tokens skipped by p_recover across the whole parse.
    // Hard-capped (PARSE_FIX_CAP) so adversarial input can't re-diagnose
    // forever; past the cap, recovery degrades to a silent single-advance.
    uint32_t      fix_count;
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
//
// `pool` is the SAME StringPool the lexer interned into — required so
// the parser can pre-intern its contextual-keyword table once and use
// StrId-equality (single u32 compare) for every keyword check instead
// of memcmp'ing the source bytes. Borrowed for the duration of the call.
GreenNode *parse_file_green(const Vec *tokens, const char *source,
                             StringPool *pool, NodeCache *cache,
                             Vec *out_errors);


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
// Error recovery — rowan-style error nodes.
//
// Composable sync-set bits naming the tokens that terminate a recovery
// skip in a given context (e.g. a list passes its separator + close
// delimiter). The SEMI/RBRACE bits are virtual-aware (they stop on the
// layout-emitted SK_VIRTUAL_SEMI / SK_VIRTUAL_RBRACE too).
typedef enum {
    SYNC_SEMI     = 1u << 0,  // SK_SEMI    (virtual-aware)
    SYNC_RBRACE   = 1u << 1,  // SK_RBRACE  (virtual-aware)
    SYNC_RPAREN   = 1u << 2,  // SK_RPAREN
    SYNC_RBRACKET = 1u << 3,  // SK_RBRACKET
    SYNC_GT       = 1u << 4,  // SK_GT
    SYNC_COMMA    = 1u << 5,  // SK_COMMA
    SYNC_PIPE     = 1u << 6,  // SK_PIPE
} SyncSet;

// Record ONE diagnostic at the current (first-bad) token, then wrap the
// unexpected token run in an SK_ERROR_NODE, skipping forward until a token
// in `sync` (or EOF) is reached. Skipped tokens flow into the error node, so
// the tree stays lossless. ALWAYS consumes >= 1 token (guaranteed progress;
// the error node is never empty), and is bounded by a global PARSE_FIX_CAP.
void         p_recover(Parser *p, SyncSet sync, const char *msg);


// ---------------------------------------------------------------------
// Contextual-keyword recognition — StrId-equality only.
//
// The lexer interns every IDENT into `pool`; the parser pre-interns its
// contextual-keyword table (`p->kws`) once at parse_file_green init.
// So every "is this token the literal X?" check is one u32 compare
// against `t->string_id.idx`, never a memcmp on source bytes.
//
// Usage:
//   p_at_kw    (p, p->kws.named)      — peek; non-consuming
//   p_at_kw_at (p, off, p->kws.ctl)   — peek at offset; non-consuming
//   p_match_kw (p, p->kws.scoped)     — peek + advance if matched
//   tok_is_kw  (t, p->kws.val)        — caller already has the Token*
// ---------------------------------------------------------------------

static inline bool tok_is_kw(const Token *t, StrId kw) {
    return t && t->kind == SK_IDENT && t->string_id.idx == kw.idx;
}

static inline bool p_at_kw(const Parser *p, StrId kw) {
    return tok_is_kw(p_current(p), kw);
}

static inline bool p_at_kw_at(const Parser *p, uint32_t off, StrId kw) {
    return tok_is_kw(p_token_at(p, off), kw);
}

static inline bool p_match_kw(Parser *p, StrId kw) {
    if (!p_at_kw(p, kw)) return false;
    p_advance(p);
    return true;
}


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
