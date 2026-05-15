#ifndef ORE_PARSER_H
#define ORE_PARSER_H

#include <stdint.h>
#include <stdbool.h>

#include "../db/workspace/module_info.h"
#include "../db/db.h"
#include "../lexer/token.h"
#include "./ast.h"

// Forward declaration of DiagBag
struct DiagBag;

// -----------------------------------------------------------------------------
// Core Parser State
// -----------------------------------------------------------------------------
typedef struct {
    struct ModuleInfo *mod;      // Destination for AST and side-tables
    const Vec *tokens;           // Vec<Token> (the layout-normalized real tokens)
    uint32_t pos;                // Current token index
    struct DiagBag *diags;
} Parser;

// Core driver function called by the DB query engine (QUERY_MODULE_AST)
void parse_module(struct ModuleInfo *mod, const Vec *tokens, struct DiagBag *diags);

// -----------------------------------------------------------------------------
// Internal Cursor Primitives (used by parse_expr, parse_decl, parse_stmt)
// -----------------------------------------------------------------------------

// Returns true if the parser is at EOF.
bool p_is_eof(const Parser *p);

// Returns the kind of the token at the current position.
TokenKind p_peek(const Parser *p);

// Returns the kind of the token `offset` ahead of the current position.
TokenKind p_peek_at(const Parser *p, uint32_t offset);

// Returns the full token struct at the current position.
const Token* p_current(const Parser *p);

// Advances the cursor by 1 and returns the previous token.
const Token* p_advance(Parser *p);

// If the current token matches `kind`, advances and returns true.
bool p_match(Parser *p, TokenKind kind);

// If the current token matches `kind`, advances and returns the token. 
// Otherwise reports a syntax error and returns NULL.
const Token* p_consume(Parser *p, TokenKind kind, const char *err_msg);

// Emits an error diagnostic at the current token's location.
void p_error(Parser *p, const char *msg);

// -----------------------------------------------------------------------------
// Internal Node Construction
// -----------------------------------------------------------------------------

// Helper to construct a TinySpan spanning from the start token to the end token.
static inline TinySpan p_span(const Parser *p, const Token *start_tok, const Token *end_tok) {
    return span_make_range((uint16_t)p->mod->file.idx, start_tok->start, end_tok->byte_end);
}

// Pushes a node to the ASTStore and simultaneously records its span in the ModuleInfo.
// `main_token` is the index of the primary token (e.g. the operator for binary ops).
AstNodeId p_push_node(Parser *p, AstNodeKind kind, uint32_t main_token, AstNodeData data, TinySpan span);

#endif // ORE_PARSER_H