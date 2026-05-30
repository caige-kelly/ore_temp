#include "./ast_expr.h"

#define DEFINE_CAST(Type, Kind)                                                \
  bool Type##_cast(const SyntaxNode *n, Type *out) {                           \
    if (!n || syntax_node_kind(n) != (Kind))                                   \
      return false;                                                            \
    out->syntax = (SyntaxNode *)n;                                             \
    return true;                                                               \
  }

DEFINE_CAST(BinExpr, SK_BIN_EXPR)
DEFINE_CAST(AssignExpr, SK_ASSIGN_EXPR)
DEFINE_CAST(PrefixExpr, SK_PREFIX_EXPR)
DEFINE_CAST(PostfixExpr, SK_POSTFIX_EXPR)
DEFINE_CAST(CallExpr, SK_CALL_EXPR)
DEFINE_CAST(IndexExpr, SK_INDEX_EXPR)
DEFINE_CAST(SliceExpr, SK_SLICE_EXPR)
DEFINE_CAST(FieldExpr, SK_FIELD_EXPR)
DEFINE_CAST(PathExpr, SK_PATH_EXPR)
DEFINE_CAST(RefExpr, SK_REF_EXPR)
DEFINE_CAST(ParenExpr, SK_PAREN_EXPR)
DEFINE_CAST(Literal, SK_LITERAL_EXPR)
DEFINE_CAST(BlockExpr, SK_BLOCK_EXPR)
DEFINE_CAST(IfExpr, SK_IF_EXPR)
DEFINE_CAST(LoopExpr, SK_LOOP_EXPR)
DEFINE_CAST(SwitchExpr, SK_SWITCH_EXPR)
DEFINE_CAST(LambdaExpr, SK_LAMBDA_EXPR)
DEFINE_CAST(HandleExpr, SK_HANDLE_EXPR)
DEFINE_CAST(HandlerExpr, SK_HANDLER_EXPR)
DEFINE_CAST(MaskExpr, SK_MASK_EXPR)
DEFINE_CAST(ProductExpr, SK_PRODUCT_EXPR)
DEFINE_CAST(EnumRefExpr, SK_ENUM_REF_EXPR)
DEFINE_CAST(BuiltinExpr, SK_BUILTIN_EXPR)
DEFINE_CAST(InitField, SK_INIT_FIELD)
DEFINE_CAST(OpClause, SK_OP_CLAUSE)
DEFINE_CAST(ReturnClause, SK_RETURN_CLAUSE)
DEFINE_CAST(InitiallyClause, SK_INITIALLY_CLAUSE)
DEFINE_CAST(FinallyClause, SK_FINALLY_CLAUSE)

// ---- Predicates used by ast_first_*_pred ----------------------------

static bool is_expr_node(OreSyntaxKind k) { return ore_kind_is_expr_node(k); }
static bool is_type_node(OreSyntaxKind k) { return ore_kind_is_type_node(k); }
static bool is_bin_op(OreSyntaxKind k) { return ore_kind_is_bin_op_token(k); }
static bool is_assign_op(OreSyntaxKind k) {
  return ore_kind_is_assign_op_token(k);
}
static bool is_prefix_op(OreSyntaxKind k) {
  return ore_kind_is_prefix_op_token(k);
}
static bool is_postfix_op(OreSyntaxKind k) {
  return ore_kind_is_postfix_op_token(k);
}
static bool is_literal_tok(OreSyntaxKind k) {
  return ore_kind_is_literal_token(k) || k == SK_TRUE_KW || k == SK_FALSE_KW ||
         k == SK_NIL_KW;
}

// ---- Internal helpers: nth typed expr/block child -------------------

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

static SyntaxNode *nth_block_or_if(SyntaxNode *n, uint32_t nth) {
  // For IfExpr's then/else branches: the parse model emits blocks
  // (SK_BLOCK_STMT / SK_BLOCK_EXPR) or — for `else if` chains — a
  // nested SK_IF_EXPR. We accept any of the three.
  uint32_t num = syntax_node_num_children(n);
  uint32_t seen = 0;
  for (uint32_t i = 0; i < num; i++) {
    SyntaxElement el = syntax_node_child_or_token(n, i);
    if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      SyntaxKind k = syntax_node_kind(el.node);
      if (k == SK_BLOCK_STMT || k == SK_BLOCK_EXPR || k == SK_IF_EXPR) {
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

// ---- BinExpr --------------------------------------------------------

SyntaxNode *BinExpr_lhs(const BinExpr *b) { return nth_expr(b->syntax, 0); }
SyntaxNode *BinExpr_rhs(const BinExpr *b) { return nth_expr(b->syntax, 1); }
SyntaxToken *BinExpr_op(const BinExpr *b) {
  return ast_first_token_pred(b->syntax, is_bin_op);
}
SyntaxKind BinExpr_op_kind(const BinExpr *b) {
  SyntaxToken *t = BinExpr_op(b);
  if (!t)
    return SYNTAX_KIND_NONE;
  SyntaxKind k = syntax_token_kind(t);
  syntax_token_release(t);
  return k;
}

// ---- AssignExpr -----------------------------------------------------

SyntaxNode *AssignExpr_lhs(const AssignExpr *a) {
  return nth_expr(a->syntax, 0);
}
SyntaxNode *AssignExpr_rhs(const AssignExpr *a) {
  return nth_expr(a->syntax, 1);
}
SyntaxToken *AssignExpr_op(const AssignExpr *a) {
  return ast_first_token_pred(a->syntax, is_assign_op);
}
SyntaxKind AssignExpr_op_kind(const AssignExpr *a) {
  SyntaxToken *t = AssignExpr_op(a);
  if (!t)
    return SYNTAX_KIND_NONE;
  SyntaxKind k = syntax_token_kind(t);
  syntax_token_release(t);
  return k;
}

// ---- PrefixExpr -----------------------------------------------------

SyntaxToken *PrefixExpr_op(const PrefixExpr *p) {
  return ast_first_token_pred(p->syntax, is_prefix_op);
}
SyntaxKind PrefixExpr_op_kind(const PrefixExpr *p) {
  SyntaxToken *t = PrefixExpr_op(p);
  if (!t)
    return SYNTAX_KIND_NONE;
  SyntaxKind k = syntax_token_kind(t);
  syntax_token_release(t);
  return k;
}
SyntaxNode *PrefixExpr_operand(const PrefixExpr *p) {
  return nth_expr(p->syntax, 0);
}

// ---- PostfixExpr ----------------------------------------------------

SyntaxNode *PostfixExpr_operand(const PostfixExpr *p) {
  return nth_expr(p->syntax, 0);
}
SyntaxToken *PostfixExpr_op(const PostfixExpr *p) {
  return ast_first_token_pred(p->syntax, is_postfix_op);
}
SyntaxKind PostfixExpr_op_kind(const PostfixExpr *p) {
  SyntaxToken *t = PostfixExpr_op(p);
  if (!t)
    return SYNTAX_KIND_NONE;
  SyntaxKind k = syntax_token_kind(t);
  syntax_token_release(t);
  return k;
}

// ---- CallExpr -------------------------------------------------------

SyntaxNode *CallExpr_callee(const CallExpr *c) {
  return nth_expr(c->syntax, 0);
}
SyntaxNode *CallExpr_args(const CallExpr *c) {
  return ast_first_child(c->syntax, SK_ARG_LIST);
}

// ---- IndexExpr ------------------------------------------------------

SyntaxNode *IndexExpr_base(const IndexExpr *i) {
  return nth_expr(i->syntax, 0);
}
SyntaxNode *IndexExpr_index(const IndexExpr *i) {
  return nth_expr(i->syntax, 1);
}

// ---- SliceExpr ------------------------------------------------------

SyntaxNode *SliceExpr_base(const SliceExpr *s) {
  return nth_expr(s->syntax, 0);
}
SyntaxNode *SliceExpr_lo(const SliceExpr *s) { return nth_expr(s->syntax, 1); }
SyntaxNode *SliceExpr_hi(const SliceExpr *s) { return nth_expr(s->syntax, 2); }

// ---- FieldExpr ------------------------------------------------------

SyntaxNode *FieldExpr_base(const FieldExpr *f) {
  return nth_expr(f->syntax, 0);
}
SyntaxToken *FieldExpr_field(const FieldExpr *f) {
  return ast_first_token(f->syntax, SK_IDENT);
}

// ---- RefExpr --------------------------------------------------------

SyntaxToken *RefExpr_name(const RefExpr *r) {
  return ast_first_token(r->syntax, SK_IDENT);
}

// ---- ParenExpr ------------------------------------------------------

SyntaxNode *ParenExpr_inner(const ParenExpr *p) {
  return nth_expr(p->syntax, 0);
}

// ---- Literal --------------------------------------------------------

SyntaxToken *Literal_token(const Literal *l) {
  return ast_first_token_pred(l->syntax, is_literal_tok);
}
SyntaxKind Literal_kind(const Literal *l) {
  SyntaxToken *t = Literal_token(l);
  if (!t)
    return SYNTAX_KIND_NONE;
  SyntaxKind k = syntax_token_kind(t);
  syntax_token_release(t);
  return k;
}

bool ast_string_literal_text(const SyntaxNode *n, const char **out_text,
                             uint32_t *out_len) {
  Literal lit;
  if (!Literal_cast(n, &lit) || Literal_kind(&lit) != SK_STRING_LIT)
    return false;
  SyntaxToken *tok = Literal_token(&lit);
  if (!tok)
    return false;
  const char *txt = syntax_token_text(tok); // green-owned; outlives tok
  uint32_t len = syntax_token_text_range(tok).length;
  syntax_token_release(tok);
  if (len >= 2 && txt[0] == '"' && txt[len - 1] == '"') {
    *out_text = txt + 1;
    *out_len = len - 2;
  } else {
    *out_text = txt;
    *out_len = len;
  }
  return true;
}

// ---- BlockExpr ------------------------------------------------------

SyntaxNode *BlockExpr_stmts(const BlockExpr *b) {
  return ast_first_child(b->syntax, SK_STMT_LIST);
}

// ---- IfExpr ---------------------------------------------------------

SyntaxNode *IfExpr_condition(const IfExpr *i) { return nth_expr(i->syntax, 0); }
SyntaxNode *IfExpr_then_branch(const IfExpr *i) {
  // First prefer block-or-if (the common `if (c) { ... } else { ... }`
  // shape) — preserves existing single-branch / else-if-chain matching.
  // Fall back to the n-th expression child counting from the cond — for
  // bare-expr branches `if (c) X else Y`, child 0 is cond, child 1 is
  // then, child 2 is else.
  SyntaxNode *r = nth_block_or_if(i->syntax, 0);
  if (r) return r;
  return nth_expr(i->syntax, 1);
}
SyntaxNode *IfExpr_else_branch(const IfExpr *i) {
  SyntaxNode *r = nth_block_or_if(i->syntax, 1);
  if (r) return r;
  return nth_expr(i->syntax, 2);
}

// ---- LoopExpr -------------------------------------------------------

SyntaxNode *LoopExpr_body(const LoopExpr *l) {
  SyntaxNode *b = ast_first_child(l->syntax, SK_BLOCK_STMT);
  if (b)
    return b;
  return ast_first_child(l->syntax, SK_BLOCK_EXPR);
}

// ---- SwitchExpr -----------------------------------------------------

SyntaxNode *SwitchExpr_scrutinee(const SwitchExpr *m) {
  return nth_expr(m->syntax, 0);
}
SyntaxNode *SwitchExpr_arms(const SwitchExpr *m) {
  return ast_first_child(m->syntax, SK_STMT_LIST);
}

// ---- LambdaExpr -----------------------------------------------------

SyntaxNode *LambdaExpr_params(const LambdaExpr *l) {
  return ast_first_child(l->syntax, SK_PARAM_LIST);
}

// Walk node children of the lambda after the param list; skip an
// optional SK_EFFECT_ROW_TYPE; return the first remaining node child
// as long as it isn't the body block. NULL when there's no return
// type (next non-skipped child IS the body block, or no more children).
SyntaxNode *LambdaExpr_return_type(const LambdaExpr *l) {
  uint32_t num = syntax_node_num_children(l->syntax);
  bool past_params = false;
  bool skipped_effect_row = false;
  for (uint32_t i = 0; i < num; i++) {
    SyntaxElement el = syntax_node_child_or_token(l->syntax, i);
    if (el.kind != SYNTAX_ELEM_NODE || !el.node) {
      if (el.kind == SYNTAX_ELEM_TOKEN && el.token)
        syntax_token_release(el.token);
      continue;
    }
    SyntaxKind k = syntax_node_kind(el.node);
    if (!past_params) {
      if (k == SK_PARAM_LIST)
        past_params = true;
      syntax_node_release(el.node);
      continue;
    }
    if (!skipped_effect_row && k == SK_EFFECT_ROW_TYPE) {
      skipped_effect_row = true;
      syntax_node_release(el.node);
      continue;
    }
    // First remaining node child after PARAM_LIST + EFFECT_ROW.
    // If it's the body block, there's no return type.
    if (k == SK_BLOCK_STMT || k == SK_BLOCK_EXPR) {
      syntax_node_release(el.node);
      return NULL;
    }
    return el.node; // caller owns +1
  }
  return NULL;
}

SyntaxNode *LambdaExpr_body(const LambdaExpr *l) {
  // The body is the trailing block. Walk children in REVERSE for
  // O(1)-ish lookup and to avoid mistaking an annotated return type
  // (a type-expression in the same node-shape) for the body.
  uint32_t num = syntax_node_num_children(l->syntax);
  for (uint32_t i = num; i > 0; i--) {
    SyntaxElement el = syntax_node_child_or_token(l->syntax, i - 1);
    if (el.kind != SYNTAX_ELEM_NODE || !el.node) {
      if (el.kind == SYNTAX_ELEM_TOKEN && el.token)
        syntax_token_release(el.token);
      continue;
    }
    SyntaxKind k = syntax_node_kind(el.node);
    if (k == SK_BLOCK_STMT || k == SK_BLOCK_EXPR)
      return el.node;
    syntax_node_release(el.node);
  }
  return NULL;
}

// ---- HandleExpr -----------------------------------------------------
//
// Nested shape: SK_HANDLE_EXPR contains [mods] HANDLE_KW [scoped]
// [<row>] LPAREN action RPAREN SK_HANDLER_EXPR{body_clauses}.
// `.body()` is the action (first expr child); `.handler()` is the
// nested SK_HANDLER_EXPR; `.effect()` is the optional row.

SyntaxNode *HandleExpr_body(const HandleExpr *h) {
  return nth_expr(h->syntax, 0);
}
SyntaxNode *HandleExpr_handler(const HandleExpr *h) {
  return ast_first_child(h->syntax, SK_HANDLER_EXPR);
}
SyntaxNode *HandleExpr_effect(const HandleExpr *h) {
  return ast_first_child(h->syntax, SK_EFFECT_ROW_TYPE);
}

// ---- HandlerExpr ----------------------------------------------------
//
// May appear standalone (with HANDLER_KW + modifiers + effect row +
// clause body) or nested inside SK_HANDLE_EXPR (clause body only).
// Accessors return NULL for absent slots in the nested form.
//
// Clauses (SK_OP_CLAUSE / SK_RETURN_CLAUSE / SK_INITIALLY_CLAUSE /
// SK_FINALLY_CLAUSE) are direct children — sema walks them via the
// generic syntax_node_first_child + next_sibling.

SyntaxNode *HandlerExpr_effect(const HandlerExpr *h) {
  return ast_first_child(h->syntax, SK_EFFECT_ROW_TYPE);
}
SyntaxNode *HandlerExpr_first_clause(const HandlerExpr *h) {
  // First child that's one of the 4 clause node kinds. Sema iterates
  // siblings filtering on kind.
  uint32_t num = syntax_node_num_children(h->syntax);
  for (uint32_t i = 0; i < num; i++) {
    SyntaxElement el = syntax_node_child_or_token(h->syntax, i);
    if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      SyntaxKind k = syntax_node_kind(el.node);
      if (k == SK_OP_CLAUSE || k == SK_RETURN_CLAUSE ||
          k == SK_INITIALLY_CLAUSE || k == SK_FINALLY_CLAUSE)
        return el.node;
      syntax_node_release(el.node);
    } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
      syntax_token_release(el.token);
    }
  }
  return NULL;
}

// ---- MaskExpr -------------------------------------------------------

SyntaxNode *MaskExpr_effect(const MaskExpr *m) {
  return ast_first_child_pred(m->syntax, is_type_node);
}
SyntaxNode *MaskExpr_body(const MaskExpr *m) { return nth_expr(m->syntax, 0); }

// ---- ProductExpr ----------------------------------------------------

SyntaxNode *ProductExpr_type(const ProductExpr *p) {
  return ast_first_child_pred(p->syntax, is_type_node);
}
SyntaxNode *ProductExpr_init(const ProductExpr *p) {
  return ast_first_child(p->syntax, SK_INIT_LIST);
}

// ---- EnumRefExpr ----------------------------------------------------

SyntaxToken *EnumRefExpr_variant(const EnumRefExpr *e) {
  return ast_first_token(e->syntax, SK_IDENT);
}

// ---- BuiltinExpr ----------------------------------------------------

SyntaxToken *BuiltinExpr_name(const BuiltinExpr *b) {
  return ast_first_token(b->syntax, SK_IDENT);
}
SyntaxNode *BuiltinExpr_args(const BuiltinExpr *b) {
  return ast_first_child(b->syntax, SK_ARG_LIST);
}

// ---- InitField ------------------------------------------------------

SyntaxToken *InitField_name(const InitField *i) {
  return ast_first_token(i->syntax, SK_IDENT);
}
SyntaxNode *InitField_value(const InitField *i) {
  return ast_first_child_pred(i->syntax, is_expr_node);
}

// ---- Handler clauses ------------------------------------------------

SyntaxToken *OpClause_name(const OpClause *c) {
  return ast_first_token(c->syntax, SK_IDENT);
}

SyntaxKind OpClause_sort_kind(const OpClause *c) {
  // The op-kind keyword is the first non-ident, non-`::`, non-`pub`
  // token after the name and `::`. For `fn` it returns SK_FN_KW;
  // ctl/val/final/raw are contextual idents — sema does a TOK_IS
  // walk over the children to disambiguate (which is why this
  // accessor returns SYNTAX_KIND_NONE for those cases).
  SyntaxToken *t = ast_first_token(c->syntax, SK_FN_KW);
  if (t) {
    SyntaxKind k = syntax_token_kind(t);
    syntax_token_release(t);
    return k;
  }
  return SYNTAX_KIND_NONE;
}

SyntaxNode *OpClause_params(const OpClause *c) {
  return ast_first_child(c->syntax, SK_PARAM_LIST);
}

SyntaxNode *OpClause_body(const OpClause *c) {
  return ast_first_child_pred(c->syntax, is_expr_node);
}

SyntaxNode *ReturnClause_params(const ReturnClause *c) {
  return ast_first_child(c->syntax, SK_PARAM_LIST);
}
SyntaxNode *ReturnClause_body(const ReturnClause *c) {
  return ast_first_child_pred(c->syntax, is_expr_node);
}

SyntaxNode *InitiallyClause_body(const InitiallyClause *c) {
  return ast_first_child_pred(c->syntax, is_expr_node);
}

SyntaxNode *FinallyClause_body(const FinallyClause *c) {
  return ast_first_child_pred(c->syntax, is_expr_node);
}
