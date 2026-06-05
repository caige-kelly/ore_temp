#include "./ast_stmt.h"

#define DEFINE_CAST(Type, Kind)                                                \
  bool Type##_cast(const SyntaxNode *n, Type *out) {                           \
    if (!n || syntax_node_kind(n) != (Kind))                                   \
      return false;                                                            \
    out->syntax = (SyntaxNode *)n;                                             \
    return true;                                                               \
  }

DEFINE_CAST(BlockStmt, SK_BLOCK_STMT)
DEFINE_CAST(ReturnStmt, SK_RETURN_STMT)
DEFINE_CAST(SwitchArm, SK_SWITCH_ARM)
DEFINE_CAST(BreakStmt, SK_BREAK_STMT)
DEFINE_CAST(ContinueStmt, SK_CONTINUE_STMT)
DEFINE_CAST(DeferStmt, SK_DEFER_STMT)
DEFINE_CAST(ExprStmt, SK_EXPR_STMT)

// A node is "expression-position-eligible" if it's a value-producing
// expression OR a block (which is itself an expression form — Slice 0
// merged SK_BLOCK_EXPR into SK_BLOCK_STMT, so a block in value position
// still has the SK_BLOCK_STMT kind that lives in the statement range).
// Used by ReturnStmt_value / DeferStmt_body / ExprStmt_expr — every
// accessor whose value-child can legally be a block under Slice 5's
// "blocks are expressions (typed void unless labeled)" model.
static bool is_expr_node(OreSyntaxKind k) {
  return ore_kind_is_expr_node(k) || k == SK_BLOCK_STMT;
}

SyntaxNode *BlockStmt_stmts(const BlockStmt *b) {
  return ast_first_child(b->syntax, SK_STMT_LIST);
}

SyntaxNode *ReturnStmt_value(const ReturnStmt *r) {
  return ast_first_child_pred(r->syntax, is_expr_node);
}

SyntaxNode *SwitchArm_pattern(const SwitchArm *a) {
  return syntax_node_first_child(a->syntax);
}
SyntaxNode *SwitchArm_body(const SwitchArm *a) {
  SyntaxNode *first = syntax_node_first_child(a->syntax);
  if (!first)
    return NULL;
  SyntaxNode *body = syntax_node_next_sibling(first);
  syntax_node_release(first);
  return body;
}

SyntaxToken *BreakStmt_label(const BreakStmt *b) {
  return ast_first_token(b->syntax, SK_IDENT);
}
SyntaxToken *ContinueStmt_label(const ContinueStmt *c) {
  return ast_first_token(c->syntax, SK_IDENT);
}

SyntaxNode *DeferStmt_body(const DeferStmt *d) {
  return ast_first_child_pred(d->syntax, is_expr_node);
}
SyntaxNode *ExprStmt_expr(const ExprStmt *e) {
  return ast_first_child_pred(e->syntax, is_expr_node);
}
