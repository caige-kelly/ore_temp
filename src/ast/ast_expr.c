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
DEFINE_CAST(IfExpr, SK_IF_EXPR)
DEFINE_CAST(LoopExpr, SK_LOOP_EXPR)
DEFINE_CAST(SwitchExpr, SK_SWITCH_EXPR)
DEFINE_CAST(HandlerExpr, SK_HANDLER_EXPR)
DEFINE_CAST(MaskExpr, SK_MASK_EXPR)
DEFINE_CAST(ProductExpr, SK_PRODUCT_EXPR)
DEFINE_CAST(EnumRefExpr, SK_ENUM_REF_EXPR)
DEFINE_CAST(BuiltinExpr, SK_BUILTIN_EXPR)
DEFINE_CAST(ComptimeExpr, SK_COMPTIME_EXPR)
DEFINE_CAST(InitField, SK_INIT_FIELD)
DEFINE_CAST(Capture, SK_CAPTURE)
DEFINE_CAST(ReturnClause, SK_RETURN_CLAUSE)

// LambdaExpr wraps ALL three lambda-shaped kinds — SK_LAMBDA_EXPR (fn),
// SK_CTL_LAMBDA (ctl op), SK_FINAL_CTL_LAMBDA (final-ctl op). They share
// one parse shape and one set of accessors (params / effect_row /
// return_type / body); the kind carries the op-sort. Consumers read the
// sort via syntax_node_kind(); navigation goes through these accessors
// regardless of sort. (Hand-written rather than DEFINE_CAST because the
// macro is single-kind.)
bool LambdaExpr_cast(const SyntaxNode *n, LambdaExpr *out) {
  if (!n || !ore_kind_is_lambda((OreSyntaxKind)syntax_node_kind(n)))
    return false;
  out->syntax = (SyntaxNode *)n;
  return true;
}

// ---- Predicates used by ast_first_*_pred ----------------------------

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
         k == SK_NIL_KW || k == SK_UNREACHABLE_KW;
}

// ---- BinExpr --------------------------------------------------------

SyntaxNode *BinExpr_lhs(const BinExpr *b) { return ast_nth_node(b->syntax, 0); }
SyntaxNode *BinExpr_rhs(const BinExpr *b) { return ast_nth_node(b->syntax, 1); }
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
  return ast_nth_node(a->syntax, 0);
}
SyntaxNode *AssignExpr_rhs(const AssignExpr *a) {
  return ast_nth_node(a->syntax, 1);
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
  return ast_nth_node(p->syntax, 0);
}

// ---- PostfixExpr ----------------------------------------------------

SyntaxNode *PostfixExpr_operand(const PostfixExpr *p) {
  return ast_nth_node(p->syntax, 0);
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
  return ast_nth_node(c->syntax, 0);
}
SyntaxNode *CallExpr_args(const CallExpr *c) {
  return ast_first_child(c->syntax, SK_ARG_LIST);
}

// ---- with-desugared call (Slice 6.12) -------------------------------
// `with x := HEAD \n rest` parses to a FLAT call carrying an SK_WITH_KW
// marker, an optional LOOSE SK_PARAM binder (`x`, a direct child BEFORE the
// head — so plain CallExpr_callee/nth-node(0) mis-picks it), the head, and an
// SK_ARG_LIST whose trailing SK_LAMBDA_EXPR is the synthetic continuation
// holding `rest`. These accessors reunite the pieces (parse_expr.c:1561).

bool CallExpr_is_with(const CallExpr *c) {
  SyntaxToken *t = ast_first_token(c->syntax, SK_WITH_KW);
  if (!t)
    return false;
  syntax_token_release(t);
  return true;
}

// `handle (action) <E> { clauses }` carries a loose HANDLE_KW marker (the
// action-first sibling of `with`'s WITH_KW). Like `with`, the head/callee is
// the SK_HANDLER_EXPR — NOT nth-node(0), which is the leading SK_ARG_LIST.
bool CallExpr_is_handle(const CallExpr *c) {
  SyntaxToken *t = ast_first_token(c->syntax, SK_HANDLE_KW);
  if (!t)
    return false;
  syntax_token_release(t);
  return true;
}

// The loose continuation binder `x` (direct SK_PARAM child), or NULL.
SyntaxNode *CallExpr_with_binder(const CallExpr *c) {
  return ast_first_child(c->syntax, SK_PARAM);
}

// The real head/callee for a loose-node call form (`with` or action-first
// `handle`): the first node child that is neither the loose binder (SK_PARAM)
// nor the SK_ARG_LIST. For `with` that's the head; for `handle` the handler.
SyntaxNode *CallExpr_head(const CallExpr *c) {
  uint32_t total = syntax_node_num_children(c->syntax);
  for (uint32_t i = 0; i < total; i++) {
    SyntaxElement el = syntax_node_child_or_token(c->syntax, i);
    if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      SyntaxKind k = syntax_node_kind(el.node);
      if (k != SK_PARAM && k != SK_ARG_LIST)
        return el.node; // caller releases
      syntax_node_release(el.node);
    } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
      syntax_token_release(el.token);
    }
  }
  return NULL;
}

// The synthetic continuation lambda — the trailing SK_LAMBDA_EXPR in the
// arg-list (holds the rest-of-block), or NULL.
SyntaxNode *CallExpr_with_continuation(const CallExpr *c) {
  SyntaxNode *args = ast_first_child(c->syntax, SK_ARG_LIST);
  if (!args)
    return NULL;
  SyntaxNode *cont = NULL;
  uint32_t total = syntax_node_num_children(args);
  for (uint32_t i = 0; i < total; i++) {
    SyntaxElement el = syntax_node_child_or_token(args, i);
    if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      if (syntax_node_kind(el.node) == SK_LAMBDA_EXPR) {
        if (cont)
          syntax_node_release(cont);
        cont = el.node; // keep the last
        continue;
      }
      syntax_node_release(el.node);
    } else if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
      syntax_token_release(el.token);
    }
  }
  syntax_node_release(args);
  return cont;
}

// ---- IndexExpr ------------------------------------------------------

SyntaxNode *IndexExpr_base(const IndexExpr *i) {
  return ast_nth_node(i->syntax, 0);
}
SyntaxNode *IndexExpr_index(const IndexExpr *i) {
  return ast_nth_node(i->syntax, 1);
}

// ---- SliceExpr ------------------------------------------------------

// The bracket body is `[ lo? RANGEOP hi? ]` — lo and hi are BOTH optional
// (`[..<hi]` is open-left, `[lo..<]` open-right), so they can't be located by
// position: an absent lo would let hi slide into lo's slot. Anchor on the
// range op (`..<` / `..=`) instead — lo is the node between `[` and the op,
// hi the node after it.
static SyntaxNode *slice_bound(SyntaxNode *n, bool want_hi) {
  uint32_t num = syntax_node_num_children(n);
  bool seen_lbracket = false, seen_op = false;
  for (uint32_t i = 0; i < num; i++) {
    SyntaxElement el = syntax_node_child_or_token(n, i);
    if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
      SyntaxKind tk = syntax_token_kind(el.token);
      syntax_token_release(el.token);
      if (tk == SK_LBRACKET)
        seen_lbracket = true;
      else if (tk == SK_DOT_DOT_LT || tk == SK_DOT_DOT_EQ)
        seen_op = true;
    } else if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      bool match = want_hi ? seen_op : (seen_lbracket && !seen_op);
      if (match)
        return el.node;
      syntax_node_release(el.node);
    }
  }
  return NULL;
}

SyntaxNode *SliceExpr_base(const SliceExpr *s) {
  return ast_nth_node(s->syntax, 0);
}
SyntaxNode *SliceExpr_lo(const SliceExpr *s) { return slice_bound(s->syntax, false); }
SyntaxNode *SliceExpr_hi(const SliceExpr *s) { return slice_bound(s->syntax, true); }

// ---- FieldExpr ------------------------------------------------------

SyntaxNode *FieldExpr_base(const FieldExpr *f) {
  return ast_nth_node(f->syntax, 0);
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
  return ast_nth_node(p->syntax, 0);
}

// ---- ComptimeExpr ---------------------------------------------------

SyntaxNode *ComptimeExpr_inner(const ComptimeExpr *c) {
  return ast_nth_node(c->syntax, 0);
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

// ---- IfExpr ---------------------------------------------------------
//
// parse_expr is pure-expression after the grammar pivot — bind decls
// (`name := expr`) cannot appear in the cond slot. The optional
// `<capture>` after `)` is a SK_CAPTURE node, retrieved separately by
// IfExpr_capture, NOT confused with the cond.

SyntaxNode *IfExpr_condition(const IfExpr *i) {
  return ast_nth_node(i->syntax, 0); // cond is always the first node child
}
SyntaxNode *IfExpr_capture(const IfExpr *i) {
  return ast_first_child(i->syntax, SK_CAPTURE);
}
SyntaxNode *IfExpr_then_branch(const IfExpr *i) {
  // The then-branch is the first node child after `)`. An optional if-let
  // capture `<x>` (SK_CAPTURE) sits between `)` and the branch, so skip it.
  // No kind filter — a bare `return`/`break` branch is found just the same.
  SyntaxNode *t = ast_first_node_after_token(i->syntax, SK_RPAREN);
  if (t && syntax_node_kind(t) == SK_CAPTURE) {
    SyntaxNode *next = syntax_node_next_sibling(t);
    syntax_node_release(t);
    t = next;
  }
  return t;
}
SyntaxNode *IfExpr_else_branch(const IfExpr *i) {
  // The else/elif tail is always the node sibling right after the then-branch:
  //   `else BODY` -> BODY (the `else` token is skipped; next NODE is BODY)
  //   `elif ...`  -> the nested SK_IF_EXPR sibling (no `else` token at all)
  //   no else     -> no next sibling -> NULL
  SyntaxNode *t = IfExpr_then_branch(i);
  if (!t)
    return NULL;
  SyntaxNode *e = syntax_node_next_sibling(t);
  syntax_node_release(t);
  return e;
}

// ---- LoopExpr -------------------------------------------------------

SyntaxNode *LoopExpr_condition(const LoopExpr *l) {
  // Optional — NULL for the infinite `loop body` form (no header `(`). The
  // cond is the first node child after `(`; token-anchored, so no kind
  // classification (the capture / body sit after `)`).
  return ast_first_node_after_token(l->syntax, SK_LPAREN);
}
SyntaxNode *LoopExpr_capture(const LoopExpr *l) {
  return ast_first_child(l->syntax, SK_CAPTURE);
}
SyntaxNode *LoopExpr_body(const LoopExpr *l) {
  // The body is the last node child before `else` (or the last node child
  // when there's no else). cond / capture / continue-expr all precede it;
  // the else branch follows the `else` token. Token-anchored so a bare-expr
  // body (`loop (c) foo()`, a one-liner) isn't missed by a block-only filter.
  uint32_t num = syntax_node_num_children(l->syntax);
  SyntaxNode *body = NULL;
  for (uint32_t i = 0; i < num; i++) {
    SyntaxElement el = syntax_node_child_or_token(l->syntax, i);
    if (el.kind == SYNTAX_ELEM_TOKEN && el.token) {
      bool is_else = (syntax_token_kind(el.token) == SK_ELSE_KW);
      syntax_token_release(el.token);
      if (is_else)
        break; // body is the last node seen before `else`
    } else if (el.kind == SYNTAX_ELEM_NODE && el.node) {
      if (body)
        syntax_node_release(body);
      body = el.node; // keep the most-recent pre-else node
    }
  }
  return body;
}
SyntaxNode *LoopExpr_continue(const LoopExpr *l) {
  return ast_first_child(l->syntax, SK_LOOP_CONTINUE);
}
// The else-body is the first node child AFTER the `else` token — token-
// anchored with NO kind filter, so a bare-expr else (`else 0`, the value
// form) is found, not just a block. (The body is the node *before* `else`.)
SyntaxNode *LoopExpr_else_branch(const LoopExpr *l) {
  return ast_first_node_after_token(l->syntax, SK_ELSE_KW);
}

// ---- Capture --------------------------------------------------------

SyntaxToken *Capture_name(const Capture *c) {
  return ast_first_token(c->syntax, SK_IDENT);
}

// ---- SwitchExpr -----------------------------------------------------

SyntaxNode *SwitchExpr_scrutinee(const SwitchExpr *m) {
  return ast_nth_node(m->syntax, 0);
}
SyntaxNode *SwitchExpr_arms(const SwitchExpr *m) {
  return ast_first_child(m->syntax, SK_STMT_LIST);
}

// ---- LambdaExpr -----------------------------------------------------

SyntaxNode *LambdaExpr_params(const LambdaExpr *l) {
  return ast_first_child(l->syntax, SK_PARAM_LIST);
}
SyntaxNode *LambdaExpr_effect_row(const LambdaExpr *l) {
  return ast_first_child(l->syntax, SK_EFFECT_ROW_TYPE);
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
    if (k == SK_BLOCK_STMT) {
      syntax_node_release(el.node);
      return NULL;
    }
    return el.node; // caller owns +1
  }
  return NULL;
}

SyntaxNode *LambdaExpr_body(const LambdaExpr *l) {
  // The body is the trailing block OR a bare expression (Slice 6 single-
  // expression sugar). Walk children in REVERSE so we find the trailing
  // node without mistaking an annotated return type (a type-kind node,
  // skipped by the predicate) for the body.
  uint32_t num = syntax_node_num_children(l->syntax);
  for (uint32_t i = num; i > 0; i--) {
    SyntaxElement el = syntax_node_child_or_token(l->syntax, i - 1);
    if (el.kind != SYNTAX_ELEM_NODE || !el.node) {
      if (el.kind == SYNTAX_ELEM_TOKEN && el.token)
        syntax_token_release(el.token);
      continue;
    }
    OreSyntaxKind k = (OreSyntaxKind)syntax_node_kind(el.node);
    if (ore_kind_is_value_node(k))
      return el.node;
    syntax_node_release(el.node);
  }
  return NULL;
}

// ---- HandlerExpr ----------------------------------------------------
//
//   handler [<row>] { clauses... }
// The optional effect row is the only token-distinguished slot; op
// clauses are ordinary binds and the `return` clause is SK_RETURN_CLAUSE.

SyntaxNode *HandlerExpr_effect(const HandlerExpr *h) {
  return ast_first_child(h->syntax, SK_EFFECT_ROW_TYPE);
}

// ---- MaskExpr -------------------------------------------------------

SyntaxNode *MaskExpr_effect(const MaskExpr *m) {
  return ast_first_child_pred(m->syntax, is_type_node);
}
// `mask <effect-row> body` — the effect-row type node is child 0, body child 1.
SyntaxNode *MaskExpr_body(const MaskExpr *m) { return ast_nth_node(m->syntax, 1); }

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
  return ast_first_child_pred(i->syntax, ore_kind_is_value_node);
}

// ---- Handler clause -------------------------------------------------

SyntaxNode *ReturnClause_params(const ReturnClause *c) {
  return ast_first_child(c->syntax, SK_PARAM_LIST);
}
SyntaxNode *ReturnClause_body(const ReturnClause *c) {
  return ast_first_child_pred(c->syntax, ore_kind_is_value_node);
}
