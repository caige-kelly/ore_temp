#include "./parse_stmt.h"
#include "./parse_expr.h"

// =====================================================================
// Statements + blocks.
// =====================================================================
//
// A statement is EITHER a bind-decl (`IDENT (::|:=|:) ...` or
// `<destructure> (::|:=) ...`) OR a single expression at PREC_NONE.
// Bind decls are STATEMENT-only — parse_expr is pure-expression and
// will not emit SK_BIND_DECL / SK_DESTRUCTURE_DECL into
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

  // Otherwise: parse as a pure expression. A trailing `,` makes it a bare
  // tuple-destructure `x, y (::|:=) value` (Slice 6.23; ore has no comma
  // operator, so the `,` is unambiguous). Single binds are stage-1 above;
  // the old `.{x,y} :=` destructure was removed in favour of the bare form
  // (`.{...}` stays a value constructor only).
  Checkpoint cp = p_checkpoint(p);
  parse_expr(p, PREC_NONE);
  if (p_peek(p) == SK_COMMA)
    parse_bare_destructure_tail(p, cp);
}

bool parse_block(Parser *p, void (*stmt_parser)(Parser *p)) {
  // Block: [:label] { stmt; stmt; ... }
  //
  // Green tree shape:
  //   SK_BLOCK_STMT
  //     [SK_LABEL]                  -- Slice 4B: optional leading label
  //     LBRACE (or VIRTUAL_LBRACE)
  //     SK_STMT_LIST
  //       <stmt_node> SEMI <stmt_node> SEMI ...
  //     RBRACE (or VIRTUAL_RBRACE)

  p_start_node(p, SK_BLOCK_STMT);

  // Slice 4B: optional leading `:label`. When present, sema treats this
  // as a labeled block — `break :label v` inside the body yields a value
  // out of the block. Lexer guarantees SK_LABEL is the no-whitespace form.
  p_match(p, SK_LABEL);

  const Token *open = p_consume(p, SK_LBRACE, "expected '{' to start block");
  if (!open) {
    p_finish_node(p);
    return false;
  }

  p_start_node(p, SK_STMT_LIST);

  while (!p_is_eof(p) && !p_check(p, SK_RBRACE)) {
    uint32_t before = p->pos;

    stmt_parser(p);

    // Slice 6.9: statement separator is OPTIONAL (Koka-style `sepEndBy`).
    // Layout still emits real / virtual `;` reliably after each statement
    // line, but absence is no longer fatal — fixes the foot-gun where
    // trailing-thunk consumption already absorbed the line's virtual-semi.
    // See [koka/src/Syntax/Parse.hs:2836](koka/src/Syntax/Parse.hs#L2836) —
    // `semis p = sepEndBy p semiColons1` — and Parse.hs:1508-1520 `block`.
    p_match(p, SK_SEMI);

    // No progress → wrap the stuck token(s) in an error node, syncing to the
    // block close (RBRACE primary; a virtual-semi can be suppressed mid-
    // continuation, so RBRACE is the reliable boundary).
    if (p->pos == before)
      p_recover_auto(p, "expected statement");
  }

  p_finish_node(p); // SK_STMT_LIST

  p_consume(p, SK_RBRACE, "expected '}' to end block");
  p_finish_node(p); // SK_BLOCK_STMT
  return true;
}

bool parse_body(Parser *p, void (*stmt_parser)(Parser *p)) {
  // Forms 1 / 2 — explicit or layout-induced braces (parse_block accepts
  // either; layout emits SK_VIRTUAL_LBRACE / VIRTUAL_RBRACE in the same
  // positions as the explicit tokens).
  // Slice 4B — labeled block: `:label { ... }` routes through parse_block
  // too, which consumes the SK_LABEL token inside the SK_BLOCK_STMT node.
  if (p_check(p, SK_LABEL) || p_check(p, SK_LBRACE) ||
      p_check(p, SK_VIRTUAL_LBRACE))
    return parse_block(p, stmt_parser);

  // Form 3 — single expression. Today's behavior: parse a bare expression
  // and leave it as-is in the parent. Slice 5 (explicit-return) will wrap
  // this in a synthetic SK_BLOCK_STMT with an implicit `return EXPR` to
  // unify the semantic value-flow model.
  parse_expr(p, PREC_NONE);
  return true;
}
