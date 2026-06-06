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


SyntaxNode *BlockStmt_stmts(const BlockStmt *b) {
  return ast_first_child(b->syntax, SK_STMT_LIST);
}

SyntaxNode *ReturnStmt_value(const ReturnStmt *r) {
  return ast_first_child_pred(r->syntax, ore_kind_is_value_node);
}

SyntaxNode *SwitchArm_patterns(const SwitchArm *a) {
  return ast_first_child(a->syntax, SK_SWITCH_PATTERN_LIST);
}
SyntaxNode *SwitchArm_pattern(const SwitchArm *a) {
  // First pattern — the first child of the SK_SWITCH_PATTERN_LIST.
  SyntaxNode *list = ast_first_child(a->syntax, SK_SWITCH_PATTERN_LIST);
  if (!list)
    return NULL;
  SyntaxNode *first = syntax_node_first_child(list);
  syntax_node_release(list);
  return first;
}
SyntaxNode *SwitchArm_body(const SwitchArm *a) {
  // The body is the first node child AFTER the `=>` token. Token-anchored
  // (LoopExpr_else_branch idiom) — the old "second node child" logic returned
  // the second PATTERN for a multi-pattern arm (`1, 2 => body`), not the body.
  uint32_t num = syntax_node_num_children(a->syntax);
  bool seen_arrow = false;
  for (uint32_t i = 0; i < num; i++) {
    SyntaxElement el = syntax_node_child_or_token(a->syntax, i);
    if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
      if (!seen_arrow && syntax_token_kind(el.token) == SK_FATARROW)
        seen_arrow = true;
      syntax_token_release(el.token);
    } else if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      if (seen_arrow)
        return el.node;
      syntax_node_release(el.node);
    }
  }
  return NULL;
}

SyntaxToken *BreakStmt_label(const BreakStmt *b) {
  return ast_first_token(b->syntax, SK_IDENT);
}
SyntaxToken *ContinueStmt_label(const ContinueStmt *c) {
  return ast_first_token(c->syntax, SK_IDENT);
}

SyntaxNode *DeferStmt_body(const DeferStmt *d) {
  return ast_first_child_pred(d->syntax, ore_kind_is_value_node);
}
SyntaxNode *ExprStmt_expr(const ExprStmt *e) {
  return ast_first_child_pred(e->syntax, ore_kind_is_value_node);
}
