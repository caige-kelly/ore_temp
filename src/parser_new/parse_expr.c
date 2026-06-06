#include "./parse_expr.h"
#include "./parse_handler.h"
#include "./parse_stmt.h"

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
// TRAILING-THUNKS + WITH
// ======================
// Slice 6 dropped the `<-` infix; juxtaposition trailing-thunks
// (`f { body }` / `f fn(x) body`) are now emitted DIRECTLY as
// `SK_CALL_EXPR { callee, SK_ARG_LIST { ..., SK_LAMBDA_EXPR } }` by
// the post-call peek and bare-LHS juxtaposition in parse_infix — no
// sema desugar is needed. `with` continuation-capture is still NOT
// desugared at parse time; the green tree carries the source verbatim
// and sema interprets the continuation-lambda binding.
//
// CONTEXTUAL KEYWORDS
// ===================
// `ctl`, `val`, `final`, `raw`, `named`, `override`, `scoped`,
// `initially`, `finally`, `in`, `behind`, `pub`, `pvt`, `abstract`,
// `distinct`, `linear` are contextual idents (SK_IDENT in the token
// stream). The parser pre-interns every contextual keyword once into
// `p->kws` at parse_file_green init time; checks happen via the
// `p_at_kw` / `p_match_kw` / `tok_is_kw` helpers in parser.h — every
// recognition is a single u32 compare against `t->string_id.idx`, never
// a memcmp of source bytes.
// =====================================================================

// ---------------------------------------------------------------------
// Precedence table.
// ---------------------------------------------------------------------

static Precedence get_infix_precedence(SyntaxKind kind) {
  switch (kind) {
  // `::` / `:=` / `:` are STATEMENT-only forms; they do NOT appear in
  // expression position. parse_stmt's peek-ahead recognizes them
  // before parse_expr ever sees them. If they show up inside a pure
  // expression, the Pratt loop returns PREC_NONE → caller errors.
  //
  // SK_LARROW (`<-`) had PREC_LAMBDA in the old trailing-lambda syntax
  // (`f <- body`). Slice 6 dropped that form in favor of juxtaposition
  // (`f { body }` / `f fn(x) body` / `f\n    body`), so `<-` has no
  // precedence — it falls through to the default PREC_NONE and the
  // Pratt loop ends. The lexer still emits the token; legacy uses now
  // surface as "unexpected `<-`" errors instead of silent parses.

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

static bool fn_lambda_body_starts(Parser *p);
static void parse_fn_lambda(Parser *p);
static void parse_lambda(Parser *p, SyntaxKind kind);
static void parse_fn_type(Parser *p);
static void parse_if_expr(Parser *p);
static void parse_loop_expr(Parser *p);
static void parse_switch_expr(Parser *p);
static void parse_aggregate_expr(Parser *p, SyntaxKind kind, bool consume_kw);
static void parse_struct_construction(Parser *p);
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
// parse_handler_expr: declared in parse_handler.h.
static void parse_mask_expr(Parser *p);
// parse_effect_row, parse_param: declared in parse_expr.h (used by
// parse_handler.c and other parser_new translation units).
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
    // `++`/`--` inc/dec.
    //
    // Slice 6: bare `T{...}` typed-construction was removed — `{` after
    // an expression is now reserved for the juxtaposition trailing-thunk
    // rule (`f { body }` calls `f` with a zero-arg lambda body). Typed
    // construction goes through the explicit `struct Name { ... }` form
    // (parse_prefix's SK_STRUCT_KW case in EXPR position) or the array-
    // literal form `[N]T{...}` (parse_bracket_expr eagerly consumes the
    // init-list).
    if (precedence < PREC_POSTFIX &&
        (tk == SK_LPAREN || tk == SK_LBRACKET || tk == SK_DOT ||
         tk == SK_CARET || tk == SK_QUESTION || tk == SK_PLUS_PLUS ||
         tk == SK_MINUS_MINUS)) {
      parse_infix(p, tk, left_cp);
      continue;
    }

    // Slice 6.9 — juxtaposition trailing-thunk (Koka-narrow).
    //
    // Matches Koka's `funapp = funblock | lambda` rule
    // ([koka/src/Syntax/Parse.hs:2156](koka/src/Syntax/Parse.hs#L2156)):
    // a trailing thunk after a value-expression is EITHER `{...}` /
    // virtual `{...}` (a zero-arg `funblock`) OR the explicit `fn(...)`
    // form (a `lambda`). Multiple chain naturally via the Pratt loop:
    // `while { cond } { body }` re-fires once per thunk, yielding a call
    // with two trailing-lambda args. Bare-expression juxtaposition
    // (`try malloc()`) is NOT a thunk — write `try { malloc() }` or
    // `try fn() malloc()` or layout `try\n    malloc()`.
    if (precedence < PREC_POSTFIX && !p->parsing_type &&
        (tk == SK_LBRACE || tk == SK_VIRTUAL_LBRACE || tk == SK_FN_KW)) {
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
  // Slice 6.18: ore has no generics, so a `<` trailing a just-parsed type is
  // the generic-application misparse (`ev<Test>`). Without this guard the
  // `<…>` silently leaks into the following body (e.g. swallows a `return`).
  // Diagnose and consume the bracket group for recovery. Safe: comparison `<`
  // is disabled under parsing_type, and effect rows occupy the
  // between-params-and-`->` slot, never the post-type position.
  if (p->parsing_type && p_peek(p) == SK_LT) {
    p_error(p,
            "generic type application is not supported (ore has no generics)");
    p_advance(p); // consume '<'
    int depth = 1;
    while (depth > 0 && !p_is_eof(p)) {
      SyntaxKind k = p_peek(p);
      if (k == SK_LT)
        depth++;
      else if (k == SK_GT)
        depth--;
      p_advance(p);
    }
  }
  p->parsing_type = saved;
}

// ---------------------------------------------------------------------
// parse_prefix — the single-token dispatcher for the prefix position.
// ---------------------------------------------------------------------

// Slice 6.18: a kind-qualified nominal type in type position —
// `struct Foo` / `union Foo` / `enum Color` / `handler Bar` / `effect Foo`.
// The `const T` analog (parse_prefix_unary), but the operand is a bare
// nominal IDENT, not a recursive type. The kind keyword IS the node kind, so
// sema dispatches on it directly and asserts the resolved decl's kind.
static void parse_qualified_type(Parser *p, SyntaxKind kind) {
  p_start_node(p, kind);
  p_advance(p); // the kind keyword (struct/union/enum/handler/effect)
  p_consume(p, SK_IDENT, "expected a type name after the type keyword");
  p_finish_node(p);
}

// Slice 6.19: `distinct <backing>` — a nominal newtype former. Unlike
// parse_qualified_type, the operand is a full backing TYPE parsed in
// TYPE-MODE (parse_type_expr ⇒ parsing_type=true), which structurally
// disables construction / trailing-thunks on the backing — this is what
// makes the old `in_distinct_rhs` flag unnecessary.
static void parse_distinct_type(Parser *p) {
  p_start_node(p, SK_DISTINCT_TYPE);
  p_advance(p); // the `distinct` contextual keyword
  parse_type_expr(p);
  p_finish_node(p);
}

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
  // `ctl` / `final-ctl` are bind-style op lambda-introducers in value
  // position. There is no `named`-on-handler dispatch — the `named`
  // keyword on handlers is gone (instance-ness = `named effect` decl +
  // `x :=` install); `named` survives only as a DECL modifier
  // (`named effect`, handled in parse_decl), never in expression prefix.
  case SK_IDENT: {
    // Bind-style op lambdas (Slice 6.16): `ctl(params) body` /
    // `final-ctl(params) body` are RHS lambda-introducers, parsed exactly
    // like `fn` but emitting the control-op node kind. Value position only
    // (a ctl-lambda is a value, never a type); sema rejects use outside a
    // handler. Single-token classification, no backtrack.
    if (!p->parsing_type) {
      if (p_at_kw(p, p->kws.ctl)) {
        parse_lambda(p, SK_CTL_LAMBDA);
        return;
      }
      if (p_at_kw(p, p->kws.final_ctl)) {
        parse_lambda(p, SK_FINAL_CTL_LAMBDA);
        return;
      }
    }
    // Slice 6.19: `distinct <backing>` in type position is the nominal
    // newtype former. In value position `distinct` stays a plain ident
    // (sema rejects the undefined identifier).
    if (p->parsing_type && p_at_kw(p, p->kws.distinct_)) {
      parse_distinct_type(p);
      return;
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
  // Slice 4B: labeled block `:blk { ... }` is also a primary expression
  // (allows `x := :blk { ...; break :blk v; }`). parse_block consumes
  // the SK_LABEL and the following block.
  case SK_LABEL:
  case SK_LBRACE:
  case SK_VIRTUAL_LBRACE:
    parse_block(p, parse_stmt);
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
    // Slice 6.18: TYPE position + name → qualified nominal type `struct Foo`.
    if (p->parsing_type && p_peek_at(p, 1) == SK_IDENT) {
      parse_qualified_type(p, SK_STRUCT_TYPE);
      return;
    }
    // Slice 6: in EXPR position with an IDENT following, this is a
    // typed-construction (`struct Point { .x = 1 }`). In TYPE position with
    // no name (`struct { ... }`), fall through to the anonymous struct-type
    // declaration parse (`Point :: struct { x : i32, y : i32 }`).
    if (!p->parsing_type && p_peek_at(p, 1) == SK_IDENT) {
      parse_struct_construction(p);
      return;
    }
    parse_aggregate_expr(p, SK_STRUCT_DECL, /*consume_kw=*/true);
    return;
  case SK_UNION_KW:
    // Slice 6.18: TYPE position + name → `union Foo`; else anon union type.
    if (p->parsing_type && p_peek_at(p, 1) == SK_IDENT) {
      parse_qualified_type(p, SK_UNION_TYPE);
      return;
    }
    parse_aggregate_expr(p, SK_UNION_DECL, /*consume_kw=*/true);
    return;
  case SK_ENUM_KW:
    // Slice 6.18: TYPE position + name → `enum Color`; else enum decl.
    if (p->parsing_type && p_peek_at(p, 1) == SK_IDENT) {
      parse_qualified_type(p, SK_ENUM_TYPE);
      return;
    }
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

  // ---- comptime prefix — wraps in SK_COMPTIME_EXPR marker -------
  // Sema uses the marker to dispatch into sema_comptime_select for
  // comptime if/switch arm selection + const-fold-required exprs.
  // The single child is the prefix expression that comptime wraps.
  case SK_COMPTIME_KW: {
    p_start_node(p, SK_COMPTIME_EXPR);
    p_advance(p); // consume `comptime` keyword
    parse_prefix(p);
    p_finish_node(p);
    return;
  }

  // ---- with / effects / handler / mask --------------------------
  case SK_WITH_KW:
    parse_with_stmt(p);
    return;
  case SK_EFFECT_KW:
    // Slice 6.18: TYPE position + name → `effect Foo` (the effect-as-type
    // form, reserved for the first-class-effect future); else effect decl.
    if (p->parsing_type && p_peek_at(p, 1) == SK_IDENT) {
      parse_qualified_type(p, SK_EFFECT_TYPE);
      return;
    }
    parse_effect_decl(p);
    return;
  case SK_HANDLER_KW:
  case SK_HANDLE_KW:
    // Slice 6.18: TYPE position → qualified handler type `handler Bar` (one
    // effect name; the `<…>` row syntax stays the value-annotation / fn-arrow
    // form, so `handler <Bar>` here is a clean "expected a type name" error).
    // Value position → the handler VALUE expression.
    if (p->parsing_type) {
      parse_qualified_type(p, SK_HANDLER_TYPE);
      return;
    }
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
    // Wrap the unexpected token(s) in an SK_ERROR_NODE up to whatever
    // delimiter the enclosing construct uses — fills the empty expression
    // slot so sema's expr walkers skip the subtree instead of choking.
    p_recover(p,
              SYNC_SEMI | SYNC_RBRACE | SYNC_RPAREN | SYNC_RBRACKET |
                  SYNC_COMMA | SYNC_GT | SYNC_PIPE,
              "expected expression");
    return;
  }
}

// Koka's `funblock` analog ([koka/src/Syntax/Parse.hs:1724-1726](koka/src/Syntax/Parse.hs#L1724)):
// a tiny wrapper that parses a trailing-thunk lambda value and emits
// the SK_LAMBDA_EXPR wrapper at the call site (not inside parse_body).
//
// Caller must have positioned the cursor on a trailing-thunk starter
// (one of SK_FN_KW / SK_LBRACE / SK_VIRTUAL_LBRACE — guaranteed by the
// Pratt-loop trigger). Caller is also responsible for opening the
// surrounding SK_ARG_LIST (or other container the lambda lands in).
//
// Emits exactly one node:
//   - SK_FN_KW           → parse_fn_lambda — full `fn(params) [-> T] body`.
//   - SK_LBRACE / virtual `{` → SK_LAMBDA_EXPR { SK_PARAM_LIST(empty), parse_body(...) }
//     — Koka's `Lam [] exp` shape.
static void emit_trailing_thunk_lambda(Parser *p) {
  if (p_peek(p) == SK_FN_KW) {
    parse_fn_lambda(p);
    return;
  }
  p_start_node(p, SK_LAMBDA_EXPR);
  p_start_node(p, SK_PARAM_LIST);
  p_finish_node(p); // empty SK_PARAM_LIST
  parse_body(p, parse_stmt);
  p_finish_node(p); // SK_LAMBDA_EXPR
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
    // Slice 6 juxtaposition: a trailing `fn(...) body` or `{ ... }` (or
    // virtual `{`) after the closing `)` is consumed as the LAST positional
    // argument of THIS call — same SK_ARG_LIST, no nested SK_CALL_EXPR.
    // Sema sees `f(a, b, fn(){...})` as a regular call with a lambda arg.
    {
      SyntaxKind nx = p_peek(p);
      if (!p->parsing_type &&
          (nx == SK_FN_KW || nx == SK_LBRACE || nx == SK_VIRTUAL_LBRACE)) {
        emit_trailing_thunk_lambda(p);
      }
    }
    p_finish_node(p); // SK_ARG_LIST
    p_finish_node(p); // SK_CALL_EXPR
    return;
  }

  // ---- Juxtaposition trailing-thunk (LHS without parens) -------
  //
  // `try { body }`, `run\n    body`, `apply fn(n) body` — wrap LHS as a
  // SK_CALL_EXPR with the trailing lambda as its sole positional arg.
  // The case `f(args) { body }` is handled INSIDE the SK_LPAREN branch
  // above (same arg list, no extra call wrapping).
  if (op_kind == SK_FN_KW || op_kind == SK_LBRACE ||
      op_kind == SK_VIRTUAL_LBRACE) {
    p_start_node_at(p, left_cp, SK_CALL_EXPR);
    p_start_node(p, SK_ARG_LIST);
    emit_trailing_thunk_lambda(p);
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

  // Slice 6 dropped the postfix `T{...}` typed-construction. Construction
  // now uses the explicit `struct Name { ... }` form in parse_prefix.
  // Array literals `[N]T{...}` are handled eagerly by parse_bracket_expr,
  // which consumes the trailing init-list as part of the array literal
  // production. `{` after any other expression flows through the post-
  // call juxtaposition handler as a trailing-thunk.

  // Bind ops (::, :=, :) — STATEMENT-only; never reached from
  // parse_expr's Pratt loop because they're absent from
  // get_infix_precedence. Destructure binds (`.{x, y} :: value`) are
  // recognized by parse_stmt after parsing the LHS pattern, then
  // delegated to parse_destructure_bind_tail directly.

  // Slice 6 dropped the `<-` trailing-lambda infix (SK_LARROW had
  // PREC_LAMBDA, emitting SK_BIN_EXPR { lhs, <-, SK_LAMBDA_EXPR }).
  // Trailing thunks now use juxtaposition (`f { body }` / `f fn(x) body`
  // / `f\n    body`) — see the post-call peek in parse_infix's postfix
  // `(` handler and the post-prefix peek in parse_expr.

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
  return tok_is_kw(t, p->kws.pub) || tok_is_kw(t, p->kws.pvt) ||
         tok_is_kw(t, p->kws.abstract_) || tok_is_kw(t, p->kws.named) ||
         tok_is_kw(t, p->kws.scoped) || tok_is_kw(t, p->kws.linear);
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
  while (at_modifier_kw(p, p_current(p))) {
    p_advance(p);
  }

  // Slice 6.19: a `distinct <backing>` RHS is a type-former, not a value —
  // route it through type-mode (like the typed-annotation path at the top)
  // so the backing can't construct. Every other RHS stays value-position.
  if (p_at_kw(p, p->kws.distinct_)) {
    parse_type_expr(p);
  } else {
    parse_expr(p, PREC_BIND);
  }
  p_finish_node(p); // SK_*_DECL
}

// ---------------------------------------------------------------------
// Helpers: optional label, single parameter.
// ---------------------------------------------------------------------

static void parse_optional_label(Parser *p) {
  // Slice 4: `:label` is now a single SK_LABEL token (lexer rule —
  // `:` immediately followed by an identifier, no whitespace). Replaces
  // the prior two-token SK_COLON + SK_IDENT path. Label names are
  // interned via the underlying token text minus the leading `:`.
  p_match(p, SK_LABEL);
}

void parse_param(Parser *p, bool name_required) {
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
  // Slice 6.10 — return takes an expression-or-block via the unified
  // body entry. The Zig-strict rule (blocks yield void unless labeled)
  // is enforced by SEMA on the SK_BLOCK_STMT child, not by the parser.
  // fn_lambda_body_starts doubles as the "value present?" predicate:
  // when the next token is a terminator, return has no value.
  if (fn_lambda_body_starts(p))
    parse_body(p, parse_stmt);
  p_finish_node(p);
}

static void parse_defer_expr(Parser *p) {
  p_start_node(p, SK_DEFER_STMT);
  p_advance(p); // defer
  parse_body(p, parse_stmt);
  p_finish_node(p);
}

static void parse_break_or_continue(Parser *p, SyntaxKind wrap_kind) {
  p_start_node(p, wrap_kind);
  p_advance(p); // break / continue
  parse_optional_label(p);
  // Slice 4C — break-with-value: `break [:label] [expr]`. Sema unifies
  // the expr type with the labeled block's result-type slot (peer-type
  // unification, Zig-style). Continue does NOT take a value — it just
  // resumes the next iteration. Same value-present predicate as `return`.
  if (wrap_kind == SK_BREAK_STMT && fn_lambda_body_starts(p)) {
    parse_body(p, parse_stmt);
  }
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
  parse_body(p, parse_stmt); // then branch (Slice 1: unified body parser)
  if (p_peek(p) == SK_ELIF_KW) {
    parse_if_expr(p); // chained as nested if
  } else if (p_match(p, SK_ELSE_KW)) {
    parse_body(p, parse_stmt); // else branch
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
  parse_body(p, parse_stmt); // loop body (Slice 2)
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
    parse_body(p, parse_stmt); // switch arm body (Slice 2)
    p_finish_node(p);          // SK_SWITCH_ARM
    p_match(p, SK_SEMI);
    if (p->pos == before)
      p_recover(p, SYNC_RBRACE | SYNC_SEMI | SYNC_PIPE, "expected switch arm");
  }
  p_finish_node(p); // SK_STMT_LIST
  p_consume(p, SK_RBRACE, "expected '}' to close switch");
  p_finish_node(p); // SK_SWITCH_EXPR
}

// ---------------------------------------------------------------------
// fn-lambda and Fn-type.
// ---------------------------------------------------------------------

// True when the lookahead is plausibly the start of a fn-lambda body.
// Slice 6: with `-> T` the sole return-type introducer, the parser no
// longer needs to disambiguate "bare return type vs body" by peeking at
// terminators. The body simply absent when the next token is a context-
// terminator (closes the enclosing expression / arg list / brace, etc.)
// and present otherwise. parse_body itself handles brace / virtual-brace
// / bare-expression dispatch.
static bool fn_lambda_body_starts(Parser *p) {
  OreSyntaxKind rk = (OreSyntaxKind)p_peek(p);
  if (rk == SK_RPAREN || rk == SK_COMMA || rk == SK_GT ||
      rk == SK_RBRACKET || rk == SK_PIPE || rk == SK_EOF)
    return false;
  if (ore_kind_is_stmt_sep(rk) || ore_kind_is_close_brace(rk))
    return false;
  return true;
}

// Generalized lambda parser. `fn` / `ctl` / `final-ctl` share ONE parse
// shape — `<kw>(params) [<effects>] [-> T] body` — differing only in the
// leading keyword token (all single tokens; `final-ctl` is kebab) and the
// emitted node `kind`. The kind tags the codegen/effect distinction for
// sema (SK_LAMBDA_EXPR = plain fn; SK_CTL_LAMBDA / SK_FINAL_CTL_LAMBDA =
// control ops). The caller has already verified the leading token.
static void parse_lambda(Parser *p, SyntaxKind kind) {
  p_start_node(p, kind);
  p_advance(p); // fn / ctl / final-ctl (single token)
  p_consume(p, SK_LPAREN, "expected '(' to start lambda parameters");
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

  // Slice 6: `-> T` is the sole return-type introducer. Without `->`,
  // the return type is inferred (from the body's value, for bare-expression
  // bodies) or void (for block bodies under Zig-strict).
  if (p_match(p, SK_RARROW))
    parse_type_expr(p);

  // Body — `{ ... }` (explicit), virtual `{ ... }` (layout-induced), or a
  // bare expression (single-expression body sugar — sema treats the value
  // as the implicit return). Absent body => fn-lambda used as a fn-type
  // value (rare; usually one would write `Fn(...)` instead).
  if (fn_lambda_body_starts(p)) {
    parse_body(p, parse_stmt);
  }
  p_finish_node(p); // `kind`
}

static void parse_fn_lambda(Parser *p) { parse_lambda(p, SK_LAMBDA_EXPR); }

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

  // Slice 6: `-> T` return-type introducer; fn-types have no body, so
  // omission of `->` simply means a void return.
  if (p_match(p, SK_RARROW))
    parse_type_expr(p);

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
    if (p_at_kw(p, p->kws.pub))
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
        p_recover(p, SYNC_RBRACE | SYNC_COMMA | SYNC_SEMI, "expected field");
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
      p_recover(p, SYNC_RBRACE | SYNC_COMMA | SYNC_SEMI, "expected field");
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
      p_recover(p, SYNC_RBRACE | SYNC_COMMA | SYNC_SEMI, "expected variant");
  }
  p_finish_node(p); // SK_VARIANT_LIST
  p_consume(p, SK_RBRACE, "expected '}' to close enum");
  p_finish_node(p); // SK_ENUM_DECL
}

// ---------------------------------------------------------------------
// Init-list helper.
//
// Consumes `{ .field = value, ... }` (positional values also accepted at
// the parser level; sema rejects per-target). Caller has positioned the
// cursor on the opening brace and opened the surrounding SK_PRODUCT_EXPR.
//
// Shared by:
//   - `[N]T{...}` array literals (parse_bracket_expr, eager).
//   - `struct Name { ... }` typed-construction (parse_struct_construction).
//   - `.{ ... }` anonymous aggregates (parse_dot_expr, separate copy
//     kept inline today; not refactored here to keep this slice tight).
// ---------------------------------------------------------------------

static void parse_init_list_body(Parser *p) {
  p_start_node(p, SK_INIT_LIST);
  p_advance(p); // `{`
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
    if (p_peek(p) == SK_DOT_DOT_DOT)
      p_advance(p);
    p_finish_node(p); // SK_INIT_FIELD
    if (!p_match(p, SK_COMMA))
      break;
    if (p->pos == before)
      p_recover(p, SYNC_RBRACE | SYNC_COMMA, "expected initializer field");
  }
  while (p_match(p, SK_SEMI)) {
  } // layout `;` before `}`
  p_consume(p, SK_RBRACE, "expected '}' to close construction");
  p_finish_node(p); // SK_INIT_LIST
}

// `struct Name { .x = 1, .y = 2 }` — typed-construction in EXPR position.
// v1: requires a plain IDENT type-name. Extending to `parse_type_expr`
// (allowing `struct Module.Foo { ... }`, `struct Map(K, V) { ... }`) is
// purely additive — defer until generics / qualified types are in sema.
//
// Resulting green tree:
//   SK_PRODUCT_EXPR
//     SK_STRUCT_KW (token — ignored by ProductExpr_type / product_expr_prefix)
//     SK_REF_EXPR  { SK_IDENT(name) }
//     SK_INIT_LIST { SK_INIT_FIELD ... }
static void parse_struct_construction(Parser *p) {
  p_start_node(p, SK_PRODUCT_EXPR);
  p_advance(p); // `struct` keyword
  p_start_node(p, SK_REF_EXPR);
  p_consume(p, SK_IDENT, "expected type name after `struct` keyword");
  p_finish_node(p); // SK_REF_EXPR
  if (!p_check(p, SK_LBRACE)) {
    p_error(p, "expected '{' to open struct construction");
    p_finish_node(p); // SK_PRODUCT_EXPR
    return;
  }
  parse_init_list_body(p);
  p_finish_node(p); // SK_PRODUCT_EXPR
}

// ---------------------------------------------------------------------
// Bracket forms — slice/array/many-pointer/inferred-array.
//
// Array literals `[_]T{...}` and `[N]T{...}` consume the trailing
// init-list eagerly (Slice 6 — postfix `T{...}` was removed). The
// `[^]T` many-pointer and `[]T` slice forms have no value-construction
// form; they only appear in type positions.
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
  // [_]T — inferred-size array; may be followed by `{ ... }` literal.
  if (p_peek_at(p, 1) == SK_UNDERSCORE && p_peek_at(p, 2) == SK_RBRACKET) {
    Checkpoint cp = p_checkpoint(p);
    p_start_node(p, SK_ARRAY_TYPE);
    p_advance(p); // [
    p_advance(p); // _
    p_advance(p); // ]
    parse_type_expr(p);
    p_finish_node(p);
    if (!p->parsing_type && p_check(p, SK_LBRACE)) {
      p_start_node_at(p, cp, SK_PRODUCT_EXPR);
      parse_init_list_body(p);
      p_finish_node(p); // SK_PRODUCT_EXPR
    }
    return;
  }
  // [N]T — sized array; may be followed by `{ ... }` literal.
  Checkpoint cp = p_checkpoint(p);
  p_start_node(p, SK_ARRAY_TYPE);
  p_advance(p); // [
  parse_expr(p, PREC_BITWISE);
  p_consume(p, SK_RBRACKET, "expected ']' after array size");
  parse_type_expr(p);
  p_finish_node(p);
  if (!p->parsing_type && p_check(p, SK_LBRACE)) {
    p_start_node_at(p, cp, SK_PRODUCT_EXPR);
    parse_init_list_body(p);
    p_finish_node(p); // SK_PRODUCT_EXPR
  }
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
        p_recover(p, SYNC_RBRACE | SYNC_COMMA, "expected initializer field");
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

void parse_effect_row(Parser *p) {
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

// Handler parsing lives in parse_handler.c — `parse_handler_expr` is
// declared in parse_handler.h. Slice 6.12+ moved the entire family
// (modifiers, op clauses, lifecycle clauses, deprecated synonyms) into
// a dedicated translation unit; the body model switched to parse_body
// (Zig-strict — explicit return).

static void parse_mask_expr(Parser *p) {
  p_start_node(p, SK_MASK_EXPR);
  p_advance(p); // mask
  (void)p_match_kw(p, p->kws.behind);
  parse_effect_row(p);
  parse_expr(p, PREC_NONE);
  p_finish_node(p);
}

static void parse_with_stmt(Parser *p) {
  // Slice 6.12 (flatten — 2026-06-05) — `with EXPR\n rest` desugars at parse
  // time to a FLAT call: `EXPR(args..., fn() { rest })`. Matches Koka's
  // `applyToContinuation` flatten path
  // ([koka/src/Syntax/Parse.hs:1655-1665](koka/src/Syntax/Parse.hs#L1655))
  // exactly: `case unParens expr of App f args -> App f (args ++ funarg)`.
  //
  // Resulting green tree (uniform for `with f`, `with f(a)`, `with x.m(a)`):
  //
  //   SK_CALL_EXPR
  //     WITH_KW                              (marker token; skipped by accessors)
  //     <callee>                             (head — postfix `(` NOT consumed here)
  //     SK_ARG_LIST                          (single, flat)
  //       <`(` <args> `)` tokens, if head had call-parens>
  //       SK_LAMBDA_EXPR                     (synthetic continuation)
  //         SK_PARAM_LIST { }                (empty — `with x = EXPR` TBD)
  //         SK_BLOCK_STMT
  //           SK_STMT_LIST
  //             <continuation statements>
  //
  // Why flat: in Koka (and now ore), the head can be ANY higher-order fn,
  // not just a handler-returning value. `with list.map\n body` means
  // `list.map(fn(x) { body })` (one arg); `with fold(init)\n body` means
  // `fold(init, fn(acc, x) { body })` (two args). Curried form would
  // demand the head ALREADY return a value taking the continuation, which
  // is a strict subset.
  //
  // Implementation: parse the callee with a CUSTOM postfix loop (allows
  // `.` `[` `^` `?` `++` `--` but BLOCKS `(` `{` `fn`) so the trailing
  // call-args fold into the outer flat SK_ARG_LIST instead of nesting.

  Checkpoint outer_cp = p_checkpoint(p);
  p_advance(p); // with

  // --- 6.17c: optional `x :=` binding (instance / CPS). ---
  // `with x := HEAD\n rest` desugars to `HEAD(fn(x){ rest })` — the
  // continuation lambda binds `x` (the instance for a `named effect`, or
  // the value a CPS callback-taker yields). Detected by a fixed 2-token
  // peek IDENT `:=` (only this shape; `x.`/`x(`/`x +` fall through to a
  // generic head). The lossless green tree keeps `x` at its SOURCE
  // position (before HEAD), so it can't live inside the end-position
  // continuation lambda — it's emitted as a LOOSE `SK_PARAM` child of the
  // call; the continuation lambda stays empty-param and sema combines
  // them (a with-call with a leading SK_PARAM → continuation binds it).
  if (p_peek(p) == SK_IDENT && p_peek_at(p, 1) == SK_COLON_EQ) {
    p_start_node(p, SK_PARAM);
    p_advance(p); // x
    p_finish_node(p); // SK_PARAM
    p_advance(p); // :=
  }

  // --- Parse HEAD. ---
  // `with` is a standalone call mechanism: it takes ANY expression head
  // (a handler value, a higher-order fn, a method, an if/switch, …) and
  // appends the rest-of-block as a trailing-lambda arg. Two head shapes
  // need handling here:
  //   - `<E>` (elided handler) → emit SK_HANDLER_EXPR directly, since
  //     parse_prefix would read `<` as a bare effect-row TYPE, not a
  //     handler. `with handler<E>{…}` (keyword) goes through parse_prefix.
  //   - everything else → parse_prefix (generic / keyword handler).
  Checkpoint head_cp = p_checkpoint(p);
  if (p_peek(p) == SK_LT) {
    p_start_node(p, SK_HANDLER_EXPR);
    parse_handler_expr_x(p); // [<eff>] { clauses } — <eff> present here
    p_finish_node(p);
  } else {
    parse_prefix(p);
    // Allow postfix DOT / LBRACKET / CARET / QUESTION / ++ / -- to bind
    // to the callee. STOP at `(` / `{` / `fn` so they fold into the
    // outer flat SK_ARG_LIST emitted below.
    for (;;) {
      SyntaxKind tk = p_peek(p);
      if (tk != SK_DOT && tk != SK_LBRACKET && tk != SK_CARET &&
          tk != SK_QUESTION && tk != SK_PLUS_PLUS && tk != SK_MINUS_MINUS)
        break;
      parse_infix(p, tk, head_cp);
    }
  }

  // --- Outer flat call: retro-wrap from outer_cp. WITH_KW + callee
  // subtree become the call's first two children. ---
  p_start_node_at(p, outer_cp, SK_CALL_EXPR);
  p_start_node(p, SK_ARG_LIST);

  // If the head was a call shape (`with f(args)` / `with x.m(args)`),
  // consume the existing arg-parens into the SAME (flat) arg list.
  if (p_match(p, SK_LPAREN)) {
    while (!p_is_eof(p) && p_peek(p) != SK_RPAREN) {
      parse_expr(p, PREC_NONE);
      if (!p_match(p, SK_COMMA))
        break;
    }
    p_consume(p, SK_RPAREN, "expected ')' after with-call arguments");
  }

  // --- Tail detection — present iff the next token is NOT a `;`
  // immediately followed by a close-brace/EOF. ---
  bool has_tail =
      !p_check(p, SK_SEMI) ||
      (!ore_kind_is_close_brace((OreSyntaxKind)p_peek_at(p, 1)) &&
       p_peek_at(p, 1) != SK_EOF);
  if (has_tail)
    p_match(p, SK_SEMI);

  // --- Append synthetic continuation lambda as final positional arg. ---
  p_start_node(p, SK_LAMBDA_EXPR);
  p_start_node(p, SK_PARAM_LIST);
  p_finish_node(p); // empty SK_PARAM_LIST
  p_start_node(p, SK_BLOCK_STMT);
  p_start_node(p, SK_STMT_LIST);
  if (has_tail) {
    while (!p_is_eof(p) && !p_check(p, SK_RBRACE)) {
      uint32_t before = p->pos;
      parse_stmt(p);
      if (p_check(p, SK_SEMI) &&
          (ore_kind_is_close_brace((OreSyntaxKind)p_peek_at(p, 1)) ||
           p_peek_at(p, 1) == SK_EOF))
        break;
      p_match(p, SK_SEMI);
      if (p->pos == before)
        p_recover(p, SYNC_RBRACE | SYNC_SEMI, "expected statement");
    }
  }
  p_finish_node(p); // SK_STMT_LIST
  p_finish_node(p); // SK_BLOCK_STMT
  p_finish_node(p); // SK_LAMBDA_EXPR

  p_finish_node(p); // SK_ARG_LIST
  p_finish_node(p); // SK_CALL_EXPR
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
  if (p_match_kw(p, p->kws.in_)) {
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
    // op-kind: fn / ctl / val / final ctl.
    // `raw ctl` is intentionally not accepted — see ParserKws comment.
    if (p_peek(p) == SK_FN_KW) {
      p_advance(p);
    } else if (p_peek(p) == SK_IDENT) {
      const Token *kt = p_current(p);
      if (tok_is_kw(kt, p->kws.ctl) || tok_is_kw(kt, p->kws.val)) {
        p_advance(p);
      } else if (tok_is_kw(kt, p->kws.final_) &&
                 p_at_kw_at(p, 1, p->kws.ctl)) {
        p_advance(p);
        p_advance(p);
      }
    } else {
      p_error(p, "expected fn/ctl/val/final ctl in op signature");
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
      p_recover(p, SYNC_RBRACE | SYNC_SEMI, "expected operation signature");
  }
  p_finish_node(p); // SK_FIELD_LIST
  p_consume(p, SK_RBRACE, "expected '}' to end effect body");
  p_finish_node(p); // SK_EFFECT_DECL
}
