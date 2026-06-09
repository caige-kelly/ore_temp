#include "./ast.h"

SyntaxNode *ast_first_child(SyntaxNode *n, SyntaxKind k) {
  return syntax_node_first_child_by_kind(n, k);
}

SyntaxToken *ast_first_token(SyntaxNode *n, SyntaxKind k) {
  SyntaxElement e = syntax_node_first_child_or_token_by_kind(n, k);
  if (e.kind == SYNTAX_ELEM_TOKEN)
    return e.token;
  // by_kind only matches what we asked for, but if the matched element
  // somehow came back as a node (shouldn't, since token kinds and node
  // kinds occupy disjoint enum ranges), drop it cleanly.
  if (e.kind == SYNTAX_ELEM_NODE && e.node)
    syntax_node_release(e.node);
  return NULL;
}

SyntaxNode *ast_nth_child(SyntaxNode *n, SyntaxKind k, uint32_t nth) {
  SyntaxNode *cur = syntax_node_first_child_by_kind(n, k);
  while (cur && nth > 0) {
    SyntaxNode *next = syntax_node_next_sibling_by_kind(cur, k);
    syntax_node_release(cur);
    cur = next;
    nth--;
  }
  return cur;
}

SyntaxToken *ast_first_token_any(SyntaxNode *n, const SyntaxKind *kinds,
                                 uint32_t count) {
  uint32_t num = syntax_node_num_children(n);
  for (uint32_t i = 0; i < num; i++) {
    SyntaxElement el = syntax_node_child_or_token(n, i);
    if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
      SyntaxKind tk = syntax_token_kind(el.token);
      for (uint32_t j = 0; j < count; j++) {
        if (kinds[j] == tk)
          return el.token;
      }
      syntax_token_release(el.token);
    } else if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      syntax_node_release(el.node);
    }
  }
  return NULL;
}

SyntaxToken *ast_first_token_pred(SyntaxNode *n, AstKindPredicate pred) {
  uint32_t num = syntax_node_num_children(n);
  for (uint32_t i = 0; i < num; i++) {
    SyntaxElement el = syntax_node_child_or_token(n, i);
    if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
      if (pred((OreSyntaxKind)syntax_token_kind(el.token)))
        return el.token;
      syntax_token_release(el.token);
    } else if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      syntax_node_release(el.node);
    }
  }
  return NULL;
}

SyntaxNode *ast_first_child_pred(SyntaxNode *n, AstKindPredicate pred) {
  uint32_t num = syntax_node_num_children(n);
  for (uint32_t i = 0; i < num; i++) {
    SyntaxElement el = syntax_node_child_or_token(n, i);
    if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      if (pred((OreSyntaxKind)syntax_node_kind(el.node)))
        return el.node;
      syntax_node_release(el.node);
    } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
      syntax_token_release(el.token);
    }
  }
  return NULL;
}

SyntaxNode *ast_nth_node(SyntaxNode *n, uint32_t nth) {
  uint32_t num = syntax_node_num_children(n);
  uint32_t seen = 0;
  for (uint32_t i = 0; i < num; i++) {
    SyntaxElement el = syntax_node_child_or_token(n, i);
    if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      if (seen == nth)
        return el.node;
      seen++;
      syntax_node_release(el.node);
    } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
      syntax_token_release(el.token);
    }
  }
  return NULL;
}

SyntaxNode *ast_first_node_after_token(SyntaxNode *n, SyntaxKind tok) {
  uint32_t num = syntax_node_num_children(n);
  bool seen_tok = false;
  for (uint32_t i = 0; i < num; i++) {
    SyntaxElement el = syntax_node_child_or_token(n, i);
    if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
      if (syntax_token_kind(el.token) == tok)
        seen_tok = true;
      syntax_token_release(el.token);
    } else if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      if (seen_tok)
        return el.node;
      syntax_node_release(el.node);
    }
  }
  return NULL;
}
