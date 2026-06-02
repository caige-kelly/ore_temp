#include "./parse_expr.h"
#include "./parse_stmt.h"

#include <string.h>

// =====================================================================
// Expression parser — Pratt, with full operator collapse and green-tree
// emission. Ported from src/parser/parse_expr.c.
//
// EMISSION MODEL
// ==============
// Each parselet emits zero or more green-tree nodes/tokens as side
// effects. Tokens emit via p_advance (which calls green_builder_token
// for every consumed token). Nodes wrap an inclusive range of emitted
// tokens via p_start_node + p_finish_node, or via the Pratt
// checkpoint/start_node_at pattern.
//
// OPERATOR COLLAPSE
// =================
// The 20 AST_EXPR_BIN_* of the old parser collapse to ONE node:
//   SK_BIN_EXPR { lhs op_token rhs }
// Sema dispatches on the op_token's kind. Same shape for SK_ASSIGN_EXPR,
// SK_PREFIX_EXPR, SK_POSTFIX_EXPR. The Pratt loop produces this via
// checkpoint + p_start_node_at after consuming the operator.
//
// DEFERRED DESUGARS
// =================
// `<-` trailing-lambda call and `with` continuation-capture are NOT
// desugared at parse time. The green tree carries the source structure
// verbatim — sema interprets `<-` as a trailing-call append and `with`
// as a continuation-lambda binding. This matches the "green tree
// preserves source" invariant.
//
// CONTEXTUAL KEYWORDS
// ===================
// `ctl`, `val`, `final`, `raw`, `named`, `override`, `scoped`,
// `initially`, `finally`, `in`, `behind`, `pub`, `pvt`, `abstract`,
// `distinct`, `linear` are contextual idents (SK_IDENT in the token
// stream). The parser checks via tok_str_eq (compares source bytes).
// =====================================================================

// True iff the token's text equals `s` exactly. `len` is the byte length
// of `s`. Used for contextual-keyword recognition.
static bool tok_str_eq(const Parser *p, const Token *t, const char *s,
                       uint32_t len) {
  if (!t || !p->source)
    return false;
  if (token_len(t) != len)
    return false;
  return memcmp(p->source + t->start, s, len) == 0;
}

#define TOK_IS(t, lit) tok_str_eq(p, (t), (lit), (uint32_t)(sizeof(lit) - 1))

// ---------------------------------------------------------------------
// Precedence table.
// ---------------------------------------------------------------------

static Precedence get_infix_precedence(SyntaxKind kind) {
  switch (kind) {
  // `::` / `:=` / `:` are STATEMENT-only forms; they do NOT appear in
  // expression position. parse_stmt's peek-ahead recognizes them
  // before parse_expr ever sees them. If they show up inside a pure
  // expression, the Pratt loop returns PREC_NONE → caller errors.

  case SK_LARROW:
    return PREC_LAMBDA;

  case SK_EQ:
  case SK_PLUS_EQ:
  case SK_MINUS_EQ:
  case SK_STAR_EQ:
  case SK_SLASH_EQ:
  case SK_PERCENT_EQ:
  case SK_AMP_EQ:
  case SK_PIPE_EQ:
  case SK_TILDE_EQ:
    return PREC_ASSIGN;

  case SK_PIPE_PIPE:
  case SK_ORELSE_KW:
    return PREC_OR;

  case SK_AMP_AMP:
    return PREC_AND;

  case SK_EQ_EQ:
  case SK_BANG_EQ:
    return PREC_EQUALITY;

  case SK_LT:
  case SK_LE:
  case SK_GT:
  case SK_GE:
    return PREC_COMPARISON;

  case SK_DOT_DOT:
    return PREC_RANGE;

  case SK_PIPE:
  case SK_AMP:
  case SK_TILDE:
    return PREC_BITWISE;

  case SK_SHL:
  case SK_SHR:
    return PREC_SHIFT;

  case SK_PLUS:
  case SK_MINUS:
    return PREC_TERM;

  case SK_STAR:
  case SK_SLASH:
  case SK_PERCENT:
    return PREC_FACTOR;

  case SK_STAR_STAR:
    return PREC_POWER;

  default:
    return PREC_NONE;
  }
}

// `**` and the assignment family are right-associative; everything else
// is left-associative.
static bool is_right_associative(SyntaxKind kind) {
  switch (kind) {
  case SK_STAR_STAR:
  case SK_EQ:
  case SK_PLUS_EQ:
  case SK_MINUS_EQ:
  case SK_STAR_EQ:
  case SK_SLASH_EQ:
  case SK_PERCENT_EQ:
  case SK_AMP_EQ:
  case SK_PIPE_EQ:
  case SK_TILDE_EQ:
    return true;
  default:
    return false;
  }
}

// ---------------------------------------------------------------------
// Forward decls.
// ---------------------------------------------------------------------

static void parse_prefix(Parser *p);
static void parse_infix(Parser *p, SyntaxKind op_kind, Checkpoint left_cp);
// parse_named_bind_decl / parse_destructure_bind_tail are EXPOSED in
// parse_expr.h — only parse_stmt invokes them. variables are statements.
static void emit_bind_decl_tail(Parser *p, SyntaxKind bind_op,
                                Checkpoint lhs_cp, bool is_destructure);

static void parse_fn_lambda(Parser *p);
static void parse_fn_type(Parser *p);
static void parse_if_expr(Parser *p);
static void parse_loop_expr(Parser *p);
static void parse_switch_expr(Parser *p);
static void parse_aggregate_expr(Parser *p, SyntaxKind kind, bool consume_kw);
static void parse_enum_expr(Parser *p);
static void parse_bracket_expr(Parser *p);
static void parse_dot_expr(Parser *p);
static void parse_builtin_expr(Parser *p);
static void parse_prefix_unary(Parser *p, SyntaxKind wrap_kind);
static void parse_return_expr(Parser *p);
static void parse_defer_expr(Parser *p);
static void parse_break_or_continue(Parser *p, SyntaxKind wrap_kind);
static void parse_with_stmt(Parser *p);
static void parse_effect_decl(Parser *p);
static void parse_handler_expr(Parser *p);
static void parse_mask_expr(Parser *p);
static void parse_effect_row(Parser *p);
static void parse_param(Parser *p, bool name_required);
static void parse_optional_label(Parser *p);

// ---------------------------------------------------------------------
// Pratt main loop.
// ---------------------------------------------------------------------

void parse_expr(Parser *p, int precedence) {
  // NOTE: bind decls (`IDENT :: VALUE` / `IDENT := VALUE` / `IDENT :
  // TYPE`) are STATEMENTS, not expressions. Handled by parse_stmt's
  // peek-ahead — see src/parser_new/parse_stmt.c. parse_expr remains
  // pure-expression so SK_VAR_DECL / SK_CONST_DECL never leak into an
  // expression slot (if-cond, loop-cond, switch-scrutinee, etc.).

  // Capture the position BEFORE parse_prefix. The Pratt loop wraps
  // every operator level via start_node_at(left_cp, ...), producing
  // left-associative trees: `a+b+c` ⇒ ((a+b)+c).
  Checkpoint left_cp = p_checkpoint(p);
  parse_prefix(p);

  for (;;) {
    SyntaxKind tk = p_peek(p);

    // Postfix forms (tighter than any binary op): `(` call,
    // `[` index/slice, `.` field access, `^` deref, `?` denil,
    // `++`/`--` inc/dec, `{` typed-construction (value position only).
    if (precedence < PREC_POSTFIX &&
        (tk == SK_LPAREN || tk == SK_LBRACKET || tk == SK_DOT ||
         tk == SK_CARET || tk == SK_QUESTION || tk == SK_PLUS_PLUS ||
         tk == SK_MINUS_MINUS ||
         (ore_kind_is_open_brace((OreSyntaxKind)tk) && !p->parsing_type &&
          !p->in_distinct_rhs))) {
      parse_infix(p, tk, left_cp);
      continue;
    }

    // Binary / assignment infix. Disabled in type position so e.g.
    // `fn() i32 -x` doesn't eat `i32 - x` as a BIN_SUB.
    if (p->parsing_type)
      break;
    Precedence prec = get_infix_precedence(tk);
    if (prec == PREC_NONE || (int)prec <= precedence)
      break;

    parse_infix(p, tk, left_cp);
  }
}

void parse_type_expr(Parser *p) {
  bool saved = p->parsing_type;
  p->parsing_type = true;
  parse_expr(p, PREC_BITWISE);
  p->parsing_type = saved;
}

// ---------------------------------------------------------------------
// parse_prefix — the single-token dispatcher for the prefix position.
// ---------------------------------------------------------------------

static void parse_prefix(Parser *p) {
  SyntaxKind kind = p_peek(p);

  switch (kind) {

  // ---- Literals -------------------------------------------------
  case SK_INT_LIT:
  case SK_FLOAT_LIT:
  case SK_STRING_LIT:
  case SK_BYTE_LIT:
  case SK_ASM_LIT:
  case SK_TRUE_KW:
  case SK_FALSE_KW:
  case SK_NIL_KW:
    p_start_node(p, SK_LITERAL_EXPR);
    p_advance(p);
    p_finish_node(p);
    return;

  // `_` as a bare expression — wildcard in patterns, discard in
  // destructure, inferred index in array literals.
  case SK_UNDERSCORE:
    p_start_node(p, SK_LITERAL_EXPR);
    p_advance(p);
    p_finish_node(p);
    return;

  // ---- Identifier reference -------------------------------------
  //
  // `named` and `override` are contextual modifiers when followed by
  // handler/handle — punt to the handler parselet (which consumes the
  // modifier itself). The redundant `named override` / `override named`
  // pair also routes here; parse_handler_expr emits the diagnostic.
  case SK_IDENT: {
    const Token *t = p_current(p);
    if ((TOK_IS(t, "named") || TOK_IS(t, "override"))) {
      SyntaxKind k1 = p_peek_at(p, 1);
      if (k1 == SK_HANDLER_KW || k1 == SK_HANDLE_KW) {
        parse_handler_expr(p);
        return;
      }
      // `named override handler` / `override named handler`.
      if (k1 == SK_IDENT) {
        const Token *t2 = p_token_at(p, 1);
        bool t2_is_mod = t2 && (TOK_IS(t2, "named") || TOK_IS(t2, "override"));
        SyntaxKind k2 = p_peek_at(p, 2);
        if (t2_is_mod && (k2 == SK_HANDLER_KW || k2 == SK_HANDLE_KW)) {
          parse_handler_expr(p);
          return;
        }
      }
    }
    // In type position (after `:` / inside `^` / `?` / `[]` / etc.),
    // emit a real type-node kind so consumer-side predicates like
    // `ore_kind_is_type_node` see this as a type, not as an expression.
    p_start_node(p, p->parsing_type ? SK_REF_TYPE : SK_REF_EXPR);
    p_advance(p);
    p_finish_node(p);
    return;
  }

  // ---- Grouping: (expr) — preserved as SK_PAREN_EXPR -----------
  case SK_LPAREN:
    p_start_node(p, SK_PAREN_EXPR);
    p_advance(p); // (
    parse_expr(p, PREC_NONE);
    p_consume(p, SK_RPAREN, "expected ')' after expression");
    p_finish_node(p);
    return;

  // ---- Block-as-expression --------------------------------------
  case SK_LBRACE:
  case SK_VIRTUAL_LBRACE:
    parse_block(p);
    return;

  // ---- Prefix unary (value position) ----------------------------
  case SK_MINUS:
  case SK_BANG:
  case SK_TILDE:
  case SK_AMP:
    parse_prefix_unary(p, SK_PREFIX_EXPR);
    return;

  // ---- Prefix unary (type position) -----------------------------
  case SK_CARET:
    parse_prefix_unary(p, SK_PTR_TYPE);
    return;
  case SK_QUESTION:
    parse_prefix_unary(p, SK_OPTIONAL_TYPE);
    return;
  case SK_CONST_KW:
    parse_prefix_unary(p, SK_CONST_TYPE);
    return;

  // ---- Decl-shaped expressions ----------------------------------
  case SK_FN_KW:
    // Lowercase `fn` starts a LAMBDA EXPRESSION. In type position
    // (e.g. `x: fn() <io> i32`), this is a category error — the user
    // almost certainly meant capital `Fn` (the function-type keyword).
    // Emit a clear syntax diag and advance past the offending token
    // so the parser doesn't silently route a lambda where a type was
    // expected.
    if (p->parsing_type) {
      p_error(p,
              "expected type; lowercase 'fn' starts a lambda expression "
              "— use 'Fn' for a function type");
      p_advance(p);
      return;
    }
    parse_fn_lambda(p);
    return;
  case SK_FN_TYPE_KW:
    parse_fn_type(p);
    return;
  case SK_STRUCT_KW:
    parse_aggregate_expr(p, SK_STRUCT_DECL, /*consume_kw=*/true);
    return;
  case SK_UNION_KW:
    parse_aggregate_expr(p, SK_UNION_DECL, /*consume_kw=*/true);
    return;
  case SK_ENUM_KW:
    parse_enum_expr(p);
    return;
  case SK_SWITCH_KW:
    parse_switch_expr(p);
    return;

  // ---- Control flow ---------------------------------------------
  case SK_IF_KW:
  case SK_ELIF_KW:
    parse_if_expr(p);
    return;
  case SK_LOOP_KW:
    parse_loop_expr(p);
    return;
  case SK_RETURN_KW:
    parse_return_expr(p);
    return;
  case SK_DEFER_KW:
    parse_defer_expr(p);
    return;
  case SK_BREAK_KW:
    parse_break_or_continue(p, SK_BREAK_STMT);
    return;
  case SK_CONTINUE_KW:
    parse_break_or_continue(p, SK_CONTINUE_STMT);
    return;

  // ---- Array / bracket forms ------------------------------------
  case SK_LBRACKET:
    parse_bracket_expr(p);
    return;

  // ---- Dot forms (.Variant, .{ ... }) ---------------------------
  case SK_DOT:
    parse_dot_expr(p);
    return;

  // ---- Compiler builtins ----------------------------------------
  case SK_AT:
    parse_builtin_expr(p);
    return;

  // ---- comptime prefix — pass-through for now -------------------
  case SK_COMPTIME_KW:
    p_advance(p);
    parse_prefix(p);
    return;

  // ---- with / effects / handler / mask --------------------------
  case SK_WITH_KW:
    parse_with_stmt(p);
    return;
  case SK_EFFECT_KW:
    parse_effect_decl(p);
    return;
  case SK_HANDLER_KW:
  case SK_HANDLE_KW:
    parse_handler_expr(p);
    return;
  case SK_MASK_KW:
    parse_mask_expr(p);
    return;

  // ---- `<` in prefix position is always an effect row -----------
  case SK_LT:
    parse_effect_row(p);
    return;

  default:
    p_error(p, "expected expression");
    p_advance(p); // forward progress
    return;
  }
}

// ---------------------------------------------------------------------
// parse_infix — postfix + binary infix.
// ---------------------------------------------------------------------

static void parse_infix(Parser *p, SyntaxKind op_kind, Checkpoint left_cp) {
  // ---- Postfix: call f(args...) ---------------------------------
  if (op_kind == SK_LPAREN) {
    p_start_node_at(p, left_cp, SK_CALL_EXPR);
    // The callee already sits at left_cp; now emit the SK_ARG_LIST
    // child containing LPAREN, args, RPAREN.
    p_start_node(p, SK_ARG_LIST);
    p_advance(p); // (
    while (!p_is_eof(p) && p_peek(p) != SK_RPAREN) {
      parse_expr(p, PREC_NONE);
      if (!p_match(p, SK_COMMA))
        break;
    }
    p_consume(p, SK_RPAREN, "expected ')' after arguments");
    p_finish_node(p); // SK_ARG_LIST
    p_finish_node(p); // SK_CALL_EXPR
    return;
  }

  // ---- Postfix: index a[i] / slice a[lo..hi] --------------------
  if (op_kind == SK_LBRACKET) {
    // The decision between INDEX_EXPR and SLICE_EXPR happens after
    // parsing `lo`. Use a SK_INDEX_EXPR placeholder via checkpoint
    // and rewrap if it turns out to be a slice.
    Checkpoint cp = left_cp; // base + the upcoming bracket form
    p_advance(p);            // [

    // `[..hi]` is rejected — Zig-strict. Consume the `..` for
    // recovery and treat as slice with NONE for lo.
    if (p_peek(p) == SK_DOT_DOT) {
      p_error(p, "open-left slice `[..hi]` not allowed; write `[0..hi]`");
      p_advance(p); // ..
      if (p_peek(p) != SK_RBRACKET)
        parse_expr(p, PREC_RANGE);
      p_consume(p, SK_RBRACKET, "expected ']' to close slice");
      p_start_node_at(p, cp, SK_SLICE_EXPR);
      p_finish_node(p);
      return;
    }

    // `..` is now a binary range op too (PREC_RANGE). Parse lo at
    // PREC_RANGE so the Pratt loop stops at `..`, leaving it for the
    // slice marker below. Otherwise `[lo..hi]` would parse lo as the
    // single expression `lo..hi`, and the slice marker would never
    // match.
    parse_expr(p, PREC_RANGE); // lo

    if (p_match(p, SK_DOT_DOT)) {
      // slice — hi at PREC_RANGE too (defensive; nested `..` is
      // semantically nonsense but no reason to mis-parse it).
      if (p_peek(p) != SK_RBRACKET)
        parse_expr(p, PREC_RANGE);
      p_consume(p, SK_RBRACKET, "expected ']' to close slice");
      p_start_node_at(p, cp, SK_SLICE_EXPR);
      p_finish_node(p);
      return;
    }

    p_consume(p, SK_RBRACKET, "expected ']' after index");
    p_start_node_at(p, cp, SK_INDEX_EXPR);
    p_finish_node(p);
    return;
  }

  // ---- Postfix: field access a.b --------------------------------
  //
  // In type position, the `.` chain is a multi-segment type path
  // (e.g., `Foo.Bar` as a type), not field access. Wrap as
  // SK_PATH_TYPE so consumers see a real type-node. The inner LHS
  // is already SK_REF_TYPE (or a nested SK_PATH_TYPE on a longer
  // chain) since parse_prefix relabels under `parsing_type`.
  if (op_kind == SK_DOT) {
    SyntaxKind wrap = p->parsing_type ? SK_PATH_TYPE : SK_FIELD_EXPR;
    p_start_node_at(p, left_cp, wrap);
    p_advance(p); // .
    p_consume(p, SK_IDENT, "expected field name after '.'");
    p_finish_node(p);
    return;
  }

  // ---- Postfix unary: ^ ? ++ -- ---------------------------------
  if (op_kind == SK_CARET || op_kind == SK_QUESTION ||
      op_kind == SK_PLUS_PLUS || op_kind == SK_MINUS_MINUS) {
    p_start_node_at(p, left_cp, SK_POSTFIX_EXPR);
    p_advance(p); // op token
    p_finish_node(p);
    return;
  }

  // ---- Typed construction T{...} or .Variant{...} ---------------
  if (ore_kind_is_open_brace((OreSyntaxKind)op_kind)) {
    p_start_node_at(p, left_cp, SK_PRODUCT_EXPR);
    // The type expr already sits at left_cp; emit the SK_INIT_LIST.
    p_start_node(p, SK_INIT_LIST);
    p_advance(p); // {
    while (!p_is_eof(p) && !p_check(p, SK_RBRACE)) {
      uint32_t before = p->pos;
      p_start_node(p, SK_INIT_FIELD);
      // Optional `.name =` prefix for named fields.
      if (p_peek(p) == SK_DOT && p_peek_at(p, 1) == SK_IDENT) {
        p_advance(p); // .
        p_advance(p); // name ident
        p_consume(p, SK_EQ, "expected '=' after field name");
      }
      parse_expr(p, PREC_NONE);
      // Array-init §C — optional postfix `...` marks this field as a
      // broadcast initializer: `[N]T{ x... }` fills all N slots with x.
      // The `...` token stays as a child of the SK_INIT_FIELD; sema
      // detects it by looking for SK_DOT_DOT_DOT among the field's
      // green-tree children and validates "broadcast must be sole
      // element" against the surrounding SK_INIT_LIST's field count.
      if (p_peek(p) == SK_DOT_DOT_DOT)
        p_advance(p);
      p_finish_node(p); // SK_INIT_FIELD
      if (!p_match(p, SK_COMMA))
        break;
      if (p->pos == before)
        p_advance(p);
    }
    while (p_match(p, SK_SEMI)) {
    } // layout `;` before `}`
    p_consume(p, SK_RBRACE, "expected '}' to close construction");
    p_finish_node(p); // SK_INIT_LIST
    p_finish_node(p); // SK_PRODUCT_EXPR
    return;
  }

  // Bind ops (::, :=, :) — STATEMENT-only; never reached from
  // parse_expr's Pratt loop because they're absent from
  // get_infix_precedence. Destructure binds (`.{x, y} :: value`) are
  // recognized by parse_stmt after parsing the LHS pattern, then
  // delegated to parse_destructure_bind_tail directly.

  // ---- Trailing-lambda `<-` -------------------------------------
  //
  // Emitted as a binary expression (SK_BIN_EXPR { lhs <- rhs }) so
  // the green tree mirrors the source. Sema desugars to a call-with-
  // trailing-lambda. The RHS is parsed as a lambda value: optional
  // `(params)` then a block.
  if (op_kind == SK_LARROW) {
    p_start_node_at(p, left_cp, SK_BIN_EXPR);
    p_advance(p); // <-
    // RHS: optional `(params)` + block, packaged as a lambda
    // value for sema to extract.
    p_start_node(p, SK_LAMBDA_EXPR);
    if (p_match(p, SK_LPAREN)) {
      p_start_node(p, SK_PARAM_LIST);
      while (!p_is_eof(p) && p_peek(p) != SK_RPAREN) {
        parse_param(p, /*name_required=*/true);
        if (!p_match(p, SK_COMMA))
          break;
      }
      p_consume(p, SK_RPAREN, "expected ')' to close lambda params");
      p_finish_node(p);
    }
    if (p_check(p, SK_LBRACE)) {
      parse_block(p);
    } else {
      p_error(p, "expected '{ ... }' (or an indented block) after '<-'");
    }
    p_finish_node(p); // SK_LAMBDA_EXPR
    p_finish_node(p); // SK_BIN_EXPR
    return;
  }

  // ---- Binary / assignment --------------------------------------
  Precedence prec = get_infix_precedence(op_kind);
  int next_prec = is_right_associative(op_kind) ? (int)prec - 1 : (int)prec;

  SyntaxKind wrap = ore_kind_is_assign_op_token((OreSyntaxKind)op_kind)
                        ? SK_ASSIGN_EXPR
                        : SK_BIN_EXPR;

  p_start_node_at(p, left_cp, wrap);
  p_advance(p); // op token
  parse_expr(p, next_prec);
  p_finish_node(p);
}

// ---------------------------------------------------------------------
// Bind decl emission.
// ---------------------------------------------------------------------
//
// Two entry points:
//
//   parse_named_bind_decl       — peek-ahead path; called from parse_expr
//                                  when `IDENT :: ...` is detected.
//   parse_destructure_bind_tail — Pratt-infix path; LHS is already a
//                                  SK_PRODUCT_EXPR pattern at left_cp,
//                                  parser sees `::`/`:=`/`:` next.
//
// Both end up at emit_bind_decl_tail which handles the optional type,
// modifier-meta words, and value parsing.

void parse_named_bind_decl(Parser *p) {
  // LHS = a bare IDENT. We retroactively wrap it in SK_CONST_DECL/
  // SK_VAR_DECL via a checkpoint.
  Checkpoint cp = p_checkpoint(p);
  p_advance(p); // IDENT — emitted to the green tree as a child
  SyntaxKind bind_op = p_peek(p);
  emit_bind_decl_tail(p, bind_op, cp, /*is_destructure=*/false);
}

void parse_destructure_bind_tail(Parser *p, SyntaxKind bind_op,
                                 Checkpoint lhs_cp) {
  // The LHS pattern (SK_PRODUCT_EXPR) is already in the green tree
  // at lhs_cp. `:`-typed binds aren't allowed for patterns; only
  // `::` and `:=`.
  if (bind_op == SK_COLON) {
    p_error(p, "destructure bind cannot use `:` (typed); use `::` or `:=`");
    // Consume the colon to make progress.
    p_advance(p);
    return;
  }
  emit_bind_decl_tail(p, bind_op, lhs_cp, /*is_destructure=*/true);
}

// Modifier-meta tokens: emitted into the green tree as-is. Sema reads
// them by walking the SK_*_DECL's children looking for known names.
static bool at_modifier_kw(const Parser *p, const Token *t) {
  if (!t)
    return false;
  if (t->kind == SK_COMPTIME_KW)
    return true;
  if (t->kind != SK_IDENT)
    return false;
  return TOK_IS(t, "pub") || TOK_IS(t, "pvt") || TOK_IS(t, "abstract") ||
         TOK_IS(t, "named") || TOK_IS(t, "scoped") || TOK_IS(t, "linear") ||
         TOK_IS(t, "distinct");
}

static void emit_bind_decl_tail(Parser *p, SyntaxKind bind_op,
                                Checkpoint lhs_cp, bool is_destructure) {
  bool is_typed = (bind_op == SK_COLON);
  bool is_const = (bind_op == SK_COLON_COLON);
  SyntaxKind wrap;
  if (is_destructure)
    wrap = SK_DESTRUCTURE_DECL;
  else if (is_const)
    wrap = SK_CONST_DECL;
  else
    wrap = SK_VAR_DECL;

  p_start_node_at(p, lhs_cp, wrap);
  p_advance(p); // bind op token (::, :=, or :)

  if (is_typed) {
    parse_type_expr(p);
    // After the type, optionally `:` (-> const) or `=` (-> var).
    if (p_match(p, SK_COLON)) {
      // `name : T : value` is const-typed.
      is_const = true;
      (void)is_const;
    } else if (p_match(p, SK_EQ)) {
      // `name : T = value` is var-typed.
    } else {
      // `name : T` (no value) is a bare type decl.
      p_finish_node(p);
      return;
    }
  }

  // Modifier run.
  bool distinct = false;
  while (at_modifier_kw(p, p_current(p))) {
    const Token *t = p_current(p);
    if (t && t->kind == SK_IDENT && TOK_IS(t, "distinct"))
      distinct = true;
    p_advance(p);
  }

  if (distinct) {
    bool saved = p->in_distinct_rhs;
    p->in_distinct_rhs = true;
    parse_expr(p, PREC_BIND);
    p->in_distinct_rhs = saved;
    // `distinct Backing { packed_fields }` — the trailing `{` is a
    // packed-body, not Backing-construction (guarded by
    // in_distinct_rhs above).
    if (p_check(p, SK_LBRACE)) {
      parse_aggregate_expr(p, SK_STRUCT_DECL, /*consume_kw=*/false);
    }
  } else {
    parse_expr(p, PREC_BIND);
  }
  p_finish_node(p); // SK_*_DECL
}

// ---------------------------------------------------------------------
// Helpers: optional label, single parameter.
// ---------------------------------------------------------------------

static void parse_optional_label(Parser *p) {
  if (!p_match(p, SK_COLON))
    return;
  p_consume(p, SK_IDENT, "expected label name after ':'");
}

static void parse_param(Parser *p, bool name_required) {
  p_start_node(p, SK_PARAM);
  p_match(p, SK_COMPTIME_KW);
  if (name_required) {
    if (p_consume(p, SK_IDENT, "expected parameter name")) {
      if (p_match(p, SK_COLON))
        parse_type_expr(p);
    }
  } else {
    parse_type_expr(p);
  }
  p_finish_node(p);
}

// ---------------------------------------------------------------------
// Simple prefix parselets.
// ---------------------------------------------------------------------

static void parse_prefix_unary(Parser *p, SyntaxKind wrap_kind) {
  p_start_node(p, wrap_kind);
  p_advance(p); // op token
  parse_expr(p, PREC_UNARY);
  p_finish_node(p);
}

static void parse_return_expr(Parser *p) {
  p_start_node(p, SK_RETURN_STMT);
  p_advance(p); // return
  OreSyntaxKind nx = (OreSyntaxKind)p_peek(p);
  if (!ore_kind_is_stmt_sep(nx) && !ore_kind_is_close_brace(nx) &&
      nx != SK_EOF) {
    parse_expr(p, PREC_NONE);
  }
  p_finish_node(p);
}

static void parse_defer_expr(Parser *p) {
  p_start_node(p, SK_DEFER_STMT);
  p_advance(p); // defer
  parse_expr(p, PREC_NONE);
  p_finish_node(p);
}

static void parse_break_or_continue(Parser *p, SyntaxKind wrap_kind) {
  p_start_node(p, wrap_kind);
  p_advance(p); // break / continue
  parse_optional_label(p);
  p_finish_node(p);
}

// `<ident>` — optional capture binding after the closing `)` of an
// if-cond / elif-cond / while-cond / range-loop header. Wraps the three
// tokens in SK_CAPTURE so body_scopes / infer / hover can key off a
// node (standard node-typed machinery) rather than a bare token.
//
// `)<` is grammatically dead space — SK_LT only appears as an effect-row
// delimiter inside fn signatures (parsing_type context) or as the
// comparison op (which cannot start an expression after a closing
// paren). So single-token lookahead disambiguates trivially.
static bool parse_optional_capture(Parser *p) {
  if (p_peek(p) != SK_LT)
    return false;
  p_start_node(p, SK_CAPTURE);
  p_advance(p); // '<'
  p_consume(p, SK_IDENT, "expected capture name after '<'");
  p_consume(p, SK_GT, "expected '>' after capture name");
  p_finish_node(p);
  return true;
}

// ---------------------------------------------------------------------
// Control flow: if, loop, switch.
// ---------------------------------------------------------------------

static void parse_if_expr(Parser *p) {
  p_start_node(p, SK_IF_EXPR);
  p_advance(p); // if / elif
  p_consume(p, SK_LPAREN, "expected '(' after if");
  parse_expr(p, PREC_NONE);
  p_consume(p, SK_RPAREN, "expected ')' after if condition");
  parse_optional_capture(p); // optional `<x>` for optional-unwrap binding
  parse_expr(p, PREC_NONE); // then branch
  if (p_peek(p) == SK_ELIF_KW) {
    parse_if_expr(p); // chained as nested if
  } else if (p_match(p, SK_ELSE_KW)) {
    parse_expr(p, PREC_NONE);
  }
  p_finish_node(p);
}

static void parse_loop_expr(Parser *p) {
  p_start_node(p, SK_LOOP_EXPR);
  p_advance(p); // loop
  parse_optional_label(p);
  if (p_match(p, SK_LPAREN)) {
    // `loop (cond) body`            — bool while-loop.
    // `loop (opt_cond) <x> body`    — while-let; iterates while cond non-nil.
    // `loop (range) <i> body`       — range iteration; `i` bound to the
    //                                 current index each iteration.
    //
    // C-for (`loop (init; cond; step)`) is GONE — counted loops use the
    // range form; non-unit-step loops use explicit while form with the
    // induction var lifted to a preceding stmt.
    parse_expr(p, PREC_NONE);
    p_consume(p, SK_RPAREN, "expected ')' after loop header");
    parse_optional_capture(p); // optional `<x>` for unwrap / index binding
  }
  parse_expr(p, PREC_NONE); // body
  p_finish_node(p);
}

static void parse_switch_expr(Parser *p) {
  p_start_node(p, SK_SWITCH_EXPR);
  p_advance(p); // switch
  p_consume(p, SK_LPAREN, "expected '(' after switch");
  parse_expr(p, PREC_NONE); // scrutinee
  p_consume(p, SK_RPAREN, "expected ')' after switch scrutinee");
  p_consume(p, SK_LBRACE, "expected '{' to open switch body");
  p_start_node(p, SK_STMT_LIST);
  while (!p_is_eof(p) && !p_check(p, SK_RBRACE)) {
    uint32_t before = p->pos;
    p_start_node(p, SK_SWITCH_ARM);
    // Pattern alternatives separated by `|`. SK_PIPE is bound at
    // PREC_BITWISE, so PREC_OR + 1 was wrong (would absorb the `|` as
    // a bitwise-or BinExpr and the alternation would be invisible to
    // the switch-arm walker). Stop strictly above PREC_BITWISE so the
    // `|` token stays at the SK_SWITCH_ARM level for p_match below.
    for (;;) {
      parse_expr(p, PREC_BITWISE + 1);
      if (!p_match(p, SK_PIPE))
        break;
    }
    p_consume(p, SK_FATARROW, "expected '=>' in switch arm");
    parse_expr(p, PREC_NONE); // body
    p_finish_node(p);         // SK_SWITCH_ARM
    p_match(p, SK_SEMI);
    if (p->pos == before)
      p_advance(p);
  }
  p_finish_node(p); // SK_STMT_LIST
  p_consume(p, SK_RBRACE, "expected '}' to close switch");
  p_finish_node(p); // SK_SWITCH_EXPR
}

// ---------------------------------------------------------------------
// fn-lambda and Fn-type.
// ---------------------------------------------------------------------

static void parse_fn_lambda(Parser *p) {
  p_start_node(p, SK_LAMBDA_EXPR);
  p_advance(p); // fn
  p_consume(p, SK_LPAREN, "expected '(' after fn");
  p_start_node(p, SK_PARAM_LIST);
  while (!p_is_eof(p) && p_peek(p) != SK_RPAREN) {
    parse_param(p, /*name_required=*/true);
    if (!p_match(p, SK_COMMA))
      break;
  }
  p_consume(p, SK_RPAREN, "expected ')' after parameters");
  p_finish_node(p); // SK_PARAM_LIST

  // Effect row `<...>` (only legal directly after `)`).
  if (p_peek(p) == SK_LT)
    parse_effect_row(p);

  // Bare return type (no `->`). Locked rule: body is always `{ }`.
  // The return type is gated by terminator tokens.
  OreSyntaxKind rk = (OreSyntaxKind)p_peek(p);
  if (!ore_kind_is_open_brace(rk) && rk != SK_RPAREN && rk != SK_COMMA &&
      rk != SK_GT && !ore_kind_is_stmt_sep(rk) &&
      !ore_kind_is_close_brace(rk) && rk != SK_PIPE && rk != SK_EOF) {
    parse_type_expr(p);
  }

  // Body — `{ ... }` (explicit or virtual). No body => fn-type form.
  if (ore_kind_is_open_brace((OreSyntaxKind)p_peek(p))) {
    parse_block(p);
  }
  p_finish_node(p); // SK_LAMBDA_EXPR
}

static void parse_fn_type(Parser *p) {
  p_start_node(p, SK_FN_TYPE);
  p_advance(p); // Fn
  p_consume(p, SK_LPAREN, "expected '(' after Fn");
  p_start_node(p, SK_PARAM_LIST);
  while (!p_is_eof(p) && p_peek(p) != SK_RPAREN) {
    parse_param(p, /*name_required=*/false);
    if (!p_match(p, SK_COMMA))
      break;
  }
  p_consume(p, SK_RPAREN, "expected ')'");
  p_finish_node(p); // SK_PARAM_LIST

  if (p_peek(p) == SK_LT)
    parse_effect_row(p);

  OreSyntaxKind rk = (OreSyntaxKind)p_peek(p);
  if (rk != SK_RPAREN && rk != SK_COMMA && rk != SK_GT &&
      !ore_kind_is_stmt_sep(rk) && !ore_kind_is_close_brace(rk) &&
      rk != SK_RBRACKET && rk != SK_PIPE && !ore_kind_is_open_brace(rk) &&
      rk != SK_EOF) {
    parse_type_expr(p);
  }
  p_finish_node(p); // SK_FN_TYPE
}

// ---------------------------------------------------------------------
// Aggregates: struct / union / enum.
// ---------------------------------------------------------------------

static void parse_aggregate_expr(Parser *p, SyntaxKind kind, bool consume_kw) {
  p_start_node(p, kind);
  if (consume_kw)
    p_advance(p); // struct/union keyword
  p_consume(p, SK_LBRACE, "expected '{' after struct/union");
  p_start_node(p, SK_FIELD_LIST);
  while (!p_is_eof(p) && !p_check(p, SK_RBRACE)) {
    uint32_t before = p->pos;

    // The `pub` contextual modifier and the field body all become
    // children of the SK_FIELD node. Decide between anonymous-nested
    // (`pub union {...}`) and named (`pub name: T`) on a non-consuming
    // peek past an optional `pub`.
    uint32_t peek_off = 0;
    if (p_peek(p) == SK_IDENT && TOK_IS(p_current(p), "pub"))
      peek_off = 1;
    SyntaxKind after_pub = p_peek_at(p, peek_off);

    // Anonymous nested aggregate (struct { union { ... } }).
    if (after_pub == SK_STRUCT_KW || after_pub == SK_UNION_KW ||
        after_pub == SK_ENUM_KW) {
      p_start_node(p, SK_FIELD);
      if (peek_off)
        p_advance(p); // pub (now as child of SK_FIELD)
      parse_type_expr(p);
      p_finish_node(p);
      if (!p_match(p, SK_COMMA))
        p_match(p, SK_SEMI);
      if (p->pos == before)
        p_advance(p);
      continue;
    }

    p_start_node(p, SK_FIELD);
    if (peek_off)
      p_advance(p); // pub
    if (!p_consume(p, SK_IDENT, "expected field name")) {
      p_finish_node(p);
      break;
    }
    p_consume(p, SK_COLON, "expected ':' after field name");
    parse_type_expr(p);
    // Optional `= <const>` for explicit offset / default.
    if (p_match(p, SK_EQ))
      parse_expr(p, PREC_BITWISE);
    p_finish_node(p); // SK_FIELD

    if (!p_match(p, SK_COMMA))
      p_match(p, SK_SEMI);
    if (p->pos == before)
      p_advance(p);
  }
  p_finish_node(p); // SK_FIELD_LIST
  p_consume(p, SK_RBRACE, "expected '}' to close struct/union");
  p_finish_node(p); // SK_STRUCT_DECL / SK_UNION_DECL
}

static void parse_enum_expr(Parser *p) {
  p_start_node(p, SK_ENUM_DECL);
  p_advance(p); // enum
  p_consume(p, SK_LBRACE, "expected '{' after enum");
  p_start_node(p, SK_VARIANT_LIST);
  while (!p_is_eof(p) && !p_check(p, SK_RBRACE)) {
    uint32_t before = p->pos;
    p_start_node(p, SK_VARIANT);
    if (!p_consume(p, SK_IDENT, "expected variant name")) {
      p_finish_node(p);
      break;
    }
    if (p_match(p, SK_EQ))
      parse_expr(p, PREC_BITWISE);
    p_finish_node(p); // SK_VARIANT
    if (!p_match(p, SK_COMMA))
      p_match(p, SK_SEMI);
    if (p->pos == before)
      p_advance(p);
  }
  p_finish_node(p); // SK_VARIANT_LIST
  p_consume(p, SK_RBRACE, "expected '}' to close enum");
  p_finish_node(p); // SK_ENUM_DECL
}

// ---------------------------------------------------------------------
// Bracket forms — slice/array/many-pointer/inferred-array.
// ---------------------------------------------------------------------

static void parse_bracket_expr(Parser *p) {
  // [^]T — many-pointer
  if (p_peek_at(p, 1) == SK_CARET) {
    p_start_node(p, SK_MANY_PTR_TYPE);
    p_advance(p); // [
    p_advance(p); // ^
    p_consume(p, SK_RBRACKET, "expected ']' after '[^'");
    parse_type_expr(p);
    p_finish_node(p);
    return;
  }
  // []T — slice
  if (p_peek_at(p, 1) == SK_RBRACKET) {
    p_start_node(p, SK_SLICE_TYPE);
    p_advance(p); // [
    p_advance(p); // ]
    parse_type_expr(p);
    p_finish_node(p);
    return;
  }
  // [_]T — inferred-size array
  if (p_peek_at(p, 1) == SK_UNDERSCORE && p_peek_at(p, 2) == SK_RBRACKET) {
    p_start_node(p, SK_ARRAY_TYPE);
    p_advance(p); // [
    p_advance(p); // _
    p_advance(p); // ]
    parse_type_expr(p);
    p_finish_node(p);
    return;
  }
  // [N]T — sized array
  p_start_node(p, SK_ARRAY_TYPE);
  p_advance(p); // [
  parse_expr(p, PREC_BITWISE);
  p_consume(p, SK_RBRACKET, "expected ']' after array size");
  parse_type_expr(p);
  p_finish_node(p);
}

// ---------------------------------------------------------------------
// Dot forms: .{...} anonymous product, .Variant enum ref.
// ---------------------------------------------------------------------

static void parse_dot_expr(Parser *p) {
  // Look ahead one token to disambiguate.
  SyntaxKind next = p_peek_at(p, 1);
  if (next == SK_LBRACE || next == SK_VIRTUAL_LBRACE) {
    // .{ ... } anonymous product
    p_start_node(p, SK_PRODUCT_EXPR);
    p_advance(p); // .
    p_start_node(p, SK_INIT_LIST);
    p_advance(p); // {
    while (!p_is_eof(p) && !p_check(p, SK_RBRACE)) {
      uint32_t before = p->pos;
      p_start_node(p, SK_INIT_FIELD);
      if (p_peek(p) == SK_DOT && p_peek_at(p, 1) == SK_IDENT) {
        p_advance(p); // .
        p_advance(p); // name
        p_consume(p, SK_EQ, "expected '=' after field name");
      }
      parse_expr(p, PREC_NONE);
      // Array-init §C — postfix `...` broadcast marker (anonymous form).
      if (p_peek(p) == SK_DOT_DOT_DOT)
        p_advance(p);
      p_finish_node(p); // SK_INIT_FIELD
      if (!p_match(p, SK_COMMA))
        break;
      if (p->pos == before)
        p_advance(p);
    }
    while (p_match(p, SK_SEMI)) {
    }
    p_consume(p, SK_RBRACE, "expected '}' to close product literal");
    p_finish_node(p); // SK_INIT_LIST
    p_finish_node(p); // SK_PRODUCT_EXPR
    return;
  }
  if (next == SK_IDENT) {
    p_start_node(p, SK_ENUM_REF_EXPR);
    p_advance(p); // .
    p_advance(p); // ident
    p_finish_node(p);
    return;
  }
  p_error(p, "unexpected '.'");
  p_advance(p);
}

// ---------------------------------------------------------------------
// @name(args?) — compiler builtin.
// ---------------------------------------------------------------------

static void parse_builtin_expr(Parser *p) {
  p_start_node(p, SK_BUILTIN_EXPR);
  p_advance(p); // @
  if (!p_consume(p, SK_IDENT, "expected builtin name after '@'")) {
    p_finish_node(p);
    return;
  }
  if (p_match(p, SK_LPAREN)) {
    p_start_node(p, SK_ARG_LIST);
    while (!p_is_eof(p) && p_peek(p) != SK_RPAREN) {
      parse_expr(p, PREC_NONE);
      if (!p_match(p, SK_COMMA))
        break;
    }
    p_consume(p, SK_RPAREN, "expected ')' after builtin args");
    p_finish_node(p); // SK_ARG_LIST
  }
  p_finish_node(p);
}

// ---------------------------------------------------------------------
// Effects, handlers, mask, with.
//
// The parser preserves the source structure verbatim — handler clauses,
// effect rows, and the `with` continuation block are NOT desugared at
// parse time. Sema interprets the green tree (e.g. `with` consumes the
// rest of the enclosing block; sema attaches the implicit lambda).
// ---------------------------------------------------------------------

static void parse_effect_row(Parser *p) {
  p_start_node(p, SK_EFFECT_ROW_TYPE);
  p_consume(p, SK_LT, "expected '<' to start effect row");
  while (!p_is_eof(p) && p_peek(p) != SK_GT) {
    if (p_peek(p) == SK_DOT_DOT_DOT) {
      p_advance(p);
      break;
    }
    if (p_peek(p) == SK_DOT_DOT) {
      p_advance(p);
      p_consume(p, SK_IDENT, "expected effect-variable name after '..'");
      break;
    }
    parse_type_expr(p);
    if (!p_match(p, SK_COMMA))
      break;
  }
  p_consume(p, SK_GT, "expected '>' to close effect row");
  p_finish_node(p);
}

// Parse the param list + body of an op-style clause: optional `(params)`
// then an expression body. Shared by SK_OP_CLAUSE (regular handler ops),
// SK_RETURN_CLAUSE (the lifecycle `return` clause), and the val-form
// shortcut `name :: val :: value` (which calls in via a different path).
static void parse_op_clause_params_and_body(Parser *p, bool body_is_expr) {
  if (p_match(p, SK_LPAREN)) {
    p_start_node(p, SK_PARAM_LIST);
    while (!p_is_eof(p) && p_peek(p) != SK_RPAREN) {
      parse_param(p, /*name_required=*/true);
      if (!p_match(p, SK_COMMA))
        break;
    }
    p_consume(p, SK_RPAREN, "expected ')' after clause params");
    p_finish_node(p);
  }
  parse_expr(p, body_is_expr ? PREC_NONE : PREC_NONE);
}

// Parse a single handler clause. Returns the SyntaxKind of the wrapping
// node that was emitted (SK_NONE if no clause recognized).
static SyntaxKind parse_handler_clause(Parser *p) {
  SyntaxKind k = p_peek(p);

  // `return (params) { body }` lifecycle clause.
  if (k == SK_RETURN_KW) {
    p_start_node(p, SK_RETURN_CLAUSE);
    p_advance(p); // return
    parse_op_clause_params_and_body(p, /*body_is_expr=*/true);
    p_finish_node(p);
    return SK_RETURN_CLAUSE;
  }

  if (k == SK_IDENT) {
    const Token *t = p_current(p);

    // `initially expr` / `finally expr` lifecycle clauses.
    if (TOK_IS(t, "initially")) {
      p_start_node(p, SK_INITIALLY_CLAUSE);
      p_advance(p); // initially
      parse_expr(p, PREC_NONE);
      p_finish_node(p);
      return SK_INITIALLY_CLAUSE;
    }
    if (TOK_IS(t, "finally")) {
      p_start_node(p, SK_FINALLY_CLAUSE);
      p_advance(p); // finally
      parse_expr(p, PREC_NONE);
      p_finish_node(p);
      return SK_FINALLY_CLAUSE;
    }

    // Op clause: `name :: [pub] (fn|ctl|val|final ctl|raw ctl) ...`.
    if (p_peek_at(p, 1) == SK_COLON_COLON) {
      p_start_node(p, SK_OP_CLAUSE);
      p_advance(p); // name (IDENT)
      p_advance(p); // ::
      // After the leading `name ::`, mark where recovery should
      // detect "no further progress was made" — trivia threading
      // means we can't compare absolute pos deltas.
      uint32_t after_colon_colon = p->pos;

      // Optional `pub` visibility (contextual).
      if (p_peek(p) == SK_IDENT && TOK_IS(p_current(p), "pub"))
        p_advance(p);

      // Op-kind keyword: fn / ctl / val / final ctl / raw ctl.
      // Track which sort matched so we know whether the body is
      // `:: value` (val) or `(params) body` (others).
      int sort = -1; // 0=fn, 1=ctl, 2=val, 3=final, 4=raw
      if (p_peek(p) == SK_FN_KW) {
        sort = 0;
        p_advance(p);
      } else if (p_peek(p) == SK_IDENT) {
        const Token *kt = p_current(p);
        if (TOK_IS(kt, "ctl")) {
          sort = 1;
          p_advance(p);
        } else if (TOK_IS(kt, "val")) {
          sort = 2;
          p_advance(p);
        } else if ((TOK_IS(kt, "final") || TOK_IS(kt, "raw")) &&
                   p_peek_at(p, 1) == SK_IDENT) {
          const Token *kt2 = p_token_at(p, 1);
          if (kt2 && TOK_IS(kt2, "ctl")) {
            sort = TOK_IS(kt, "final") ? 3 : 4;
            p_advance(p); // final / raw
            p_advance(p); // ctl
          }
        }
      }

      if (sort < 0) {
        p_error(p, "expected fn/ctl/val/final ctl/raw ctl in op clause");
        // Try to recover by advancing one token so the outer
        // loop's forward-progress guard fires.
        if (p->pos == after_colon_colon)
          p_advance(p);
        p_finish_node(p);
        return SK_OP_CLAUSE;
      }

      if (sort == 2) {
        // val form: `name :: val :: value`.
        p_consume(p, SK_COLON_COLON, "expected '::' after 'val <name>'");
        parse_expr(p, PREC_BIND);
      } else {
        // fn / ctl / final ctl / raw ctl: (params) + expression body.
        parse_op_clause_params_and_body(p, /*body_is_expr=*/false);
      }

      p_finish_node(p); // SK_OP_CLAUSE
      return SK_OP_CLAUSE;
    }
  }

  return SYNTAX_KIND_NONE;
}

// Shared body-block parser for both SK_HANDLER_EXPR (standalone) and
// SK_HANDLE_EXPR (nested inner SK_HANDLER_EXPR). Tracks duplicates of
// the three lifecycle clauses and emits diagnostics on dup detection.
// The clauses are emitted as direct children of the currently-open node.
static void parse_handler_body(Parser *p) {
  p_consume(p, SK_LBRACE, "expected '{' to start handler body");
  bool seen_return = false, seen_initially = false, seen_finally = false;
  while (!p_is_eof(p) && !p_check(p, SK_RBRACE)) {
    uint32_t before = p->pos;
    SyntaxKind clause = parse_handler_clause(p);
    if (clause == SYNTAX_KIND_NONE) {
      p_error(p, "expected a handler operation or return/initially/finally");
      if (p->pos == before)
        p_advance(p);
      continue;
    }
    // Dup detection on lifecycle slots. Parser keeps the dup node in
    // the tree (preserves source structure); sema doesn't have to
    // re-check.
    if (clause == SK_RETURN_CLAUSE) {
      if (seen_return)
        p_error(p, "duplicate 'return' clause in handler");
      seen_return = true;
    } else if (clause == SK_INITIALLY_CLAUSE) {
      if (seen_initially)
        p_error(p, "duplicate 'initially' clause in handler");
      seen_initially = true;
    } else if (clause == SK_FINALLY_CLAUSE) {
      if (seen_finally)
        p_error(p, "duplicate 'finally' clause in handler");
      seen_finally = true;
    }
    p_consume(p, SK_SEMI, "expected ';' after handler clause");
  }
  p_consume(p, SK_RBRACE, "expected '}' to end handler body");
}

// Consume optional `named` / `override` modifier (one token each). Also
// handles the redundant `named override` / `override named` combination
// the old parser tolerates with an error. Emitted as token children of
// the wrapping SK_HANDLER_EXPR / SK_HANDLE_EXPR.
static void parse_handler_modifiers(Parser *p) {
  if (p_peek(p) != SK_IDENT)
    return;
  const Token *t = p_current(p);
  bool first_is_mod = TOK_IS(t, "named") || TOK_IS(t, "override");
  if (!first_is_mod)
    return;
  p_advance(p);
  if (p_peek(p) == SK_IDENT) {
    const Token *t2 = p_current(p);
    bool t2_is_mod = TOK_IS(t2, "named") || TOK_IS(t2, "override");
    // Old parser: `named override` / `override named` are mutually
    // exclusive; diagnose but recover by consuming both.
    if (t2_is_mod) {
      p_error(p, "'named' and 'override' are mutually exclusive on a handler");
      p_advance(p);
    }
  }
}

static void parse_handler_expr(Parser *p) {
  // Determine whether the upcoming construct is a standalone `handler`
  // or a `handle` (which wraps an action). Optional `named` / `override`
  // modifier may precede the keyword.
  uint32_t offset = 0;
  if (p_peek(p) == SK_IDENT) {
    const Token *t = p_current(p);
    if (TOK_IS(t, "named") || TOK_IS(t, "override"))
      offset = 1;
    // `named override` / `override named` — peek past both.
    if (offset == 1 && p_peek_at(p, 1) == SK_IDENT) {
      const Token *t2 = p_token_at(p, 1);
      if (t2 && (TOK_IS(t2, "named") || TOK_IS(t2, "override")))
        offset = 2;
    }
  }
  bool is_handle = p_peek_at(p, offset) == SK_HANDLE_KW;
  SyntaxKind wrap = is_handle ? SK_HANDLE_EXPR : SK_HANDLER_EXPR;

  p_start_node(p, wrap);

  // 1. Optional modifiers (named / override).
  parse_handler_modifiers(p);

  // 2. handle / handler keyword.
  if (p_peek(p) == SK_HANDLE_KW || p_peek(p) == SK_HANDLER_KW) {
    p_advance(p);
  } else {
    p_error(p, "expected 'handler' or 'handle'");
  }

  // 3. Optional `scoped` contextual modifier.
  if (p_peek(p) == SK_IDENT && TOK_IS(p_current(p), "scoped"))
    p_advance(p);

  // 4. Optional effect row.
  if (p_peek(p) == SK_LT)
    parse_effect_row(p);

  if (is_handle) {
    // 5a. `( action )` between effect row and body.
    p_consume(p, SK_LPAREN, "expected '(' after 'handle'");
    parse_expr(p, PREC_NONE);
    p_consume(p, SK_RPAREN, "expected ')' after handle action");

    // 5b. Nested inner SK_HANDLER_EXPR wrapping just the body
    // clauses (no kw / row / modifiers — those live on the outer
    // SK_HANDLE_EXPR). Mirrors the old AST_EXPR_HANDLE.bin = (handler,
    // action) shape; HandleExpr.handler() returns this nested node.
    p_start_node(p, SK_HANDLER_EXPR);
    parse_handler_body(p);
    p_finish_node(p); // inner SK_HANDLER_EXPR
  } else {
    // Standalone handler: body clauses are direct children.
    parse_handler_body(p);
  }

  p_finish_node(p); // outer SK_HANDLER_EXPR or SK_HANDLE_EXPR
}

static void parse_mask_expr(Parser *p) {
  p_start_node(p, SK_MASK_EXPR);
  p_advance(p); // mask
  if (p_peek(p) == SK_IDENT && TOK_IS(p_current(p), "behind"))
    p_advance(p);
  parse_effect_row(p);
  parse_expr(p, PREC_NONE);
  p_finish_node(p);
}

// True when the cursor sits at a bare-op-clause start — the `with ctl
// panic() { ... }` / `with return (r) { ... }` shorthand for a 1-clause
// handler. Ports old parser's `at_bare_op_clause`. Non-consuming.
static bool at_bare_op_clause(Parser *p) {
  SyntaxKind k = p_peek(p);
  if (k == SK_RETURN_KW)
    return true;
  if (k == SK_FN_KW)
    return p_peek_at(p, 1) == SK_IDENT && p_peek_at(p, 2) == SK_LPAREN;
  if (k != SK_IDENT)
    return false;
  const Token *t = p_current(p);
  return TOK_IS(t, "ctl") || TOK_IS(t, "val") || TOK_IS(t, "final") ||
         TOK_IS(t, "raw") || TOK_IS(t, "initially") || TOK_IS(t, "finally");
}

static void parse_with_stmt(Parser *p) {
  // The `with` statement consumes the rest of the enclosing block as
  // its continuation tail. The green tree preserves this verbatim:
  //
  //   SK_HANDLE_EXPR (reused as the with-statement carrier)
  //     WITH_KW
  //     [ SK_PARAM (binder) `:=` ]?
  //     <head_expr>
  //     ;
  //     <continuation_block>
  //
  // Sema reads this and desugars to the trailing-call form.
  p_start_node(p, SK_HANDLE_EXPR);
  p_advance(p); // with

  // Optional `binder :=` (name only, no type).
  if (p_peek(p) == SK_IDENT && p_peek_at(p, 1) == SK_COLON_EQ) {
    parse_param(p, /*name_required=*/true);
    p_consume(p, SK_COLON_EQ, "expected ':=' after with-binder");
  }

  // Head expression — with bare-op-clause shortcut:
  // `with ctl panic() { ... }` synthesizes a 1-clause SK_HANDLER_EXPR
  // so sema's desugar (with → trailing-call) treats it identically to
  // the explicit `with handler { ctl panic() { ... } }` form.
  if (at_bare_op_clause(p)) {
    p_start_node(p, SK_HANDLER_EXPR);
    if (parse_handler_clause(p) == SYNTAX_KIND_NONE) {
      p_error(p, "expected a handler operation after 'with'");
    }
    p_finish_node(p);
  } else {
    parse_expr(p, PREC_NONE);
  }

  // Consume the head→tail separator `;` UNLESS the tail is empty
  // (in which case the `;` is the with-statement's own terminator).
  // A tail is "present" if the next non-`;` token isn't a close-brace
  // or EOF.
  if (!p_check(p, SK_SEMI) ||
      (!ore_kind_is_close_brace((OreSyntaxKind)p_peek_at(p, 1)) &&
       p_peek_at(p, 1) != SK_EOF)) {
    p_match(p, SK_SEMI);

    // Continuation tail = remaining statements in the enclosing block.
    // CRITICAL: the LAST `;` (the with-statement's own terminator) must
    // be left for the outer parse_block. Old-parser parity: parse a
    // statement, THEN check at_block_terminator; if true, BREAK without
    // consuming the `;`.
    p_start_node(p, SK_BLOCK_STMT);
    p_start_node(p, SK_STMT_LIST);
    while (!p_is_eof(p) && !p_check(p, SK_RBRACE)) {
      uint32_t before = p->pos;
      parse_expr(p, PREC_NONE);
      // After parsing one statement, if we're sitting on `;}` or `;<eof>`,
      // leave the `;` for the enclosing block's terminator-consume.
      if (p_check(p, SK_SEMI) &&
          (ore_kind_is_close_brace((OreSyntaxKind)p_peek_at(p, 1)) ||
           p_peek_at(p, 1) == SK_EOF))
        break;
      p_match(p, SK_SEMI);
      if (p->pos == before)
        p_advance(p);
    }
    p_finish_node(p); // SK_STMT_LIST
    p_finish_node(p); // SK_BLOCK_STMT
  }
  p_finish_node(p); // SK_HANDLE_EXPR (the with-statement carrier)
}

static void parse_effect_decl(Parser *p) {
  p_start_node(p, SK_EFFECT_DECL);
  p_advance(p); // effect

  // Optional `< T1, T2 >` type parameters.
  if (p_match(p, SK_LT)) {
    while (!p_is_eof(p) && p_peek(p) != SK_GT) {
      parse_type_expr(p);
      if (!p_match(p, SK_COMMA))
        break;
    }
    p_consume(p, SK_GT, "expected '>' after effect type parameters");
  }

  // Optional `in Type`.
  if (p_peek(p) == SK_IDENT && TOK_IS(p_current(p), "in")) {
    p_advance(p);
    parse_type_expr(p);
  }

  // Body: `{ name :: op_kind(params) return_type ; ... }`.
  p_consume(p, SK_LBRACE, "expected '{' to start effect body");
  p_start_node(p, SK_FIELD_LIST);
  while (!p_is_eof(p) && !p_check(p, SK_RBRACE)) {
    uint32_t before = p->pos;
    p_start_node(p, SK_FIELD);
    p_consume(p, SK_IDENT, "expected operation name");
    p_consume(p, SK_COLON_COLON, "expected '::' in operation signature");
    // op-kind: fn / ctl / val / final ctl / raw ctl
    if (p_peek(p) == SK_FN_KW) {
      p_advance(p);
    } else if (p_peek(p) == SK_IDENT) {
      const Token *kt = p_current(p);
      if (TOK_IS(kt, "ctl") || TOK_IS(kt, "val")) {
        p_advance(p);
      } else if ((TOK_IS(kt, "final") || TOK_IS(kt, "raw")) &&
                 p_peek_at(p, 1) == SK_IDENT) {
        const Token *kt2 = p_token_at(p, 1);
        if (kt2 && TOK_IS(kt2, "ctl")) {
          p_advance(p);
          p_advance(p);
        }
      }
    } else {
      p_error(p, "expected fn/ctl/val/final ctl/raw ctl in op signature");
    }
    // Signature: optional (params), then return type (no body).
    if (p_match(p, SK_LPAREN)) {
      p_start_node(p, SK_PARAM_LIST);
      while (!p_is_eof(p) && p_peek(p) != SK_RPAREN) {
        parse_param(p, /*name_required=*/true);
        if (!p_match(p, SK_COMMA))
          break;
      }
      p_consume(p, SK_RPAREN, "expected ')' after op params");
      p_finish_node(p);
    }
    // Bare return type (no `->`).
    OreSyntaxKind rk = (OreSyntaxKind)p_peek(p);
    if (!ore_kind_is_stmt_sep(rk) && !ore_kind_is_close_brace(rk) &&
        rk != SK_EOF && rk != SK_COMMA) {
      parse_type_expr(p);
    }
    p_finish_node(p); // SK_FIELD
    p_consume(p, SK_SEMI, "expected ';' after operation signature");
    if (p->pos == before)
      p_advance(p);
  }
  p_finish_node(p); // SK_FIELD_LIST
  p_consume(p, SK_RBRACE, "expected '}' to end effect body");
  p_finish_node(p); // SK_EFFECT_DECL
}
