#ifndef ORE_PARSER_NEW_PARSE_STMT_H
#define ORE_PARSER_NEW_PARSE_STMT_H

#include "./parser.h"

// A statement is one expression at PREC_NONE — Ore has no separate
// statement grammar. Emitted by parse_expr into the green tree; the
// caller wraps the statement-list in SK_STMT_LIST inside SK_BLOCK_STMT.
void parse_stmt(Parser *p);

// Parses a `{ ... }` block, emitting SK_BLOCK_STMT { SK_STMT_LIST } in
// the green tree. Returns true if the block opened (and finished), false
// if no opening brace was found (parser error already recorded).
bool parse_block(Parser *p);

#endif // ORE_PARSER_NEW_PARSE_STMT_H
