#include "parse_stmt.h"
#include "parse_expr.h"

// A statement is one expression at PREC_NONE (binds, control flow,
// returns, etc. are all expression forms — see parse_expr.c). The
// terminating `;` is NOT consumed here — the caller owns it, because
// the Koka-style layout pass guarantees exactly one `;` after every
// statement (including the last one before a `}`) and after every
// top-level decl. Making the terminator mandatory at the call site
// surfaces under-consumption bugs instead of masking them.
AstNodeId parse_stmt(Parser *p) { return parse_expr(p, PREC_NONE); }

AstNodeId parse_block(Parser *p) {
  uint32_t start_tok_idx = p->pos;
  const Token *start_tok =
      p_consume(p, TK_LBRACE, "Expected '{' to start block");
  if (!start_tok)
    return AST_NODE_ID_NONE;
  uint32_t op_index = p->pos - 1;

  // extras = [stmt_count, stmt0, ...] — unbounded via scratch stack.
  uint32_t st = scratch_open(p);
  uint32_t cnt_at = scratch_reserve(p);
  uint32_t stmt_count = 0;

  while (!p_is_eof(p) && p_peek(p) != TK_RBRACE) {
    uint32_t before = p->pos;

    AstNodeId stmt = parse_stmt(p);
    if (stmt.idx != 0) {
      scratch_push(p, stmt.idx);
      stmt_count++;
    }

    // Mandatory terminator: layout emits a `;` after every statement,
    // including the last one immediately before `}`.
    p_consume(p, TK_SEMI, "Expected ';' after statement");

    // Forward-progress guard: never spin on an unparseable token.
    if (p->pos == before)
      p_advance(p);
  }

  const Token *end_tok = p_consume(p, TK_RBRACE, "Expected '}' to end block");
  if (!end_tok)
    return AST_NODE_ID_NONE;

  scratch_set(p, cnt_at, stmt_count);
  AstExtraDataIdx extra = scratch_emit(p, st);
  AstNodeData data = {0};
  data.extra_idx = extra;

  return p_push_node_tok(p, AST_STMT_BLOCK, op_index, start_tok_idx, data);
}
