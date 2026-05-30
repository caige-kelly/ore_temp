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

static bool is_expr_node(OreSyntaxKind k) { return ore_kind_is_expr_node(k); }

static SyntaxNode *nth_expr(SyntaxNode *n, uint32_t nth) {
  uint32_t num = syntax_node_num_children(n);
  uint32_t seen = 0;
  for (uint32_t i = 0; i < num; i++) {
    SyntaxElement el = syntax_node_child_or_token(n, i);
    if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      if (ore_kind_is_expr_node((OreSyntaxKind)syntax_node_kind(el.node))) {
        if (seen == nth)
          return el.node;
        seen++;
      }
      syntax_node_release(el.node);
    } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
      syntax_token_release(el.token);
    }
  }
  return NULL;
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
  return nth_expr(d->syntax, 0);
}
SyntaxNode *ExprStmt_expr(const ExprStmt *e) { return nth_expr(e->syntax, 0); }
