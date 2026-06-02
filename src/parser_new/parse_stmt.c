#include "./parse_stmt.h"
#include "./parse_expr.h"

// =====================================================================
// Statements + blocks.
// =====================================================================
//
// A statement is EITHER a bind-decl (`IDENT (::|:=|:) ...` or
// `<destructure> (::|:=) ...`) OR a single expression at PREC_NONE.
// Bind decls are STATEMENT-only — parse_expr is pure-expression and
// will not emit SK_VAR_DECL / SK_CONST_DECL / SK_DESTRUCTURE_DECL into
// any expression slot. The terminating `;` is NOT consumed here — the
// caller owns it.

void parse_stmt(Parser *p) {
  // Named bind: `IDENT (::|:=|:) ...`. Peek-ahead avoids emitting the
  // IDENT as a bare SK_REF_EXPR before recognizing the bind.
  if (p_peek(p) == SK_IDENT) {
    SyntaxKind nx = p_peek_at(p, 1);
    if (nx == SK_COLON_COLON || nx == SK_COLON_EQ || nx == SK_COLON) {
      parse_named_bind_decl(p);
      return;
    }
  }

  // Otherwise: parse as a pure expression. If the parsed LHS is a
  // destructure pattern (SK_PRODUCT_EXPR), it may be followed by
  // `::` or `:=` for a destructure-bind decl. `:` (typed) destructure
  // is rejected inside parse_destructure_bind_tail itself.
  Checkpoint cp = p_checkpoint(p);
  parse_expr(p, PREC_NONE);
  SyntaxKind nx = p_peek(p);
  if (nx == SK_COLON_COLON || nx == SK_COLON_EQ) {
    parse_destructure_bind_tail(p, nx, cp);
  }
}

bool parse_block(Parser *p) {
  // Block: { stmt; stmt; ... }
  //
  // Green tree shape:
  //   SK_BLOCK_STMT
  //     LBRACE (or VIRTUAL_LBRACE)
  //     SK_STMT_LIST
  //       <stmt_node> SEMI <stmt_node> SEMI ...
  //     RBRACE (or VIRTUAL_RBRACE)

  p_start_node(p, SK_BLOCK_STMT);

  const Token *open = p_consume(p, SK_LBRACE, "expected '{' to start block");
  if (!open) {
    p_finish_node(p);
    return false;
  }

  p_start_node(p, SK_STMT_LIST);

  while (!p_is_eof(p) && !p_check(p, SK_RBRACE)) {
    uint32_t before = p->pos;

    parse_stmt(p);

    // Mandatory terminator: layout emits a `;` after every statement,
    // including the last one immediately before `}`.
    p_consume(p, SK_SEMI, "expected ';' after statement");

    // Forward-progress guard: never spin on an unparseable token.
    if (p->pos == before)
      p_advance(p);
  }

  p_finish_node(p); // SK_STMT_LIST

  p_consume(p, SK_RBRACE, "expected '}' to end block");
  p_finish_node(p); // SK_BLOCK_STMT
  return true;
}
