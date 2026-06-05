#ifndef ORE_PARSER_NEW_PARSE_STMT_H
#define ORE_PARSER_NEW_PARSE_STMT_H

#include "./parser.h"

// A statement is one expression at PREC_NONE — Ore has no separate
// statement grammar. Emitted by parse_expr into the green tree; the
// caller wraps the statement-list in SK_STMT_LIST inside SK_BLOCK_STMT.
void parse_stmt(Parser *p);

// Parses a `{ ... }` block, emitting SK_BLOCK_STMT { SK_STMT_LIST } in
// the green tree. The `stmt_parser` callback is invoked per statement
// inside the block — `parse_stmt` for regular blocks, or a context-
// specific parser for handler bodies (Slice 3) etc. Returns true if the
// block opened (and finished), false if no opening brace was found
// (parser error already recorded).
bool parse_block(Parser *p, void (*stmt_parser)(Parser *p));

// Unified body parser — the single entry point for every block-bearing
// construct (if/loop/switch/defer/handler-body/fn-lambda/trailing-lambda).
// Modeled on Zig's `bodyParseFn` callback ([Parse.zig:3518]) and Koka's
// `bodyexpr` production ([Parse.hs:1677-1683]): one grammar for body
// admits three surface forms, all producing a clean AST shape:
//
//   1. Explicit braces:   { stmt; stmt; ... }    → SK_BLOCK_STMT
//   2. Layout-induced:    newline + indent + stmts + dedent
//                         (lexer emits SK_VIRTUAL_LBRACE / VIRTUAL_RBRACE)
//   3. Single expression: expr                   → bare expression node
//
// Under Zig-strict (Slice 5), forms 1 / 2 yield void unless labeled with
// break-with-value. Form 3 (bare expression) is currently only entered
// by callers that don't gate on brace presence (if/loop/switch/defer):
// the if/switch-as-expression rules give it value-semantics via sema's
// SK_IF_EXPR / SK_SWITCH_EXPR unification.
//
// Fn-lambda and trailing-lambda bodies BRACE-GATE their parse_body call
// today — no single-expression body sugar for them yet. That sugar
// requires a body-introducer token (e.g. `->` for return type) to be
// added to the grammar first; deferred to a separate slice.
//
// `stmt_parser` is the per-statement callback used by forms 1 / 2:
// `parse_stmt` for regular blocks, `parse_handler_clause_stmt` for
// handler bodies.
bool parse_body(Parser *p, void (*stmt_parser)(Parser *p));

#endif // ORE_PARSER_NEW_PARSE_STMT_H
