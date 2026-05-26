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
static bool tok_str_eq(const Parser *p, const Token *t, const char *s, uint32_t len) {
    if (!t || !p->source) return false;
    if (token_len(t) != len) return false;
    return memcmp(p->source + t->start, s, len) == 0;
}

#define TOK_IS(t, lit)  tok_str_eq(p, (t), (lit), (uint32_t)(sizeof(lit) - 1))


// ---------------------------------------------------------------------
// Precedence table.
// ---------------------------------------------------------------------

static Precedence get_infix_precedence(SyntaxKind kind) {
    switch (kind) {
    case SK_COLON_COLON:
    case SK_COLON_EQ:
    case SK_COLON:
        return PREC_BIND;

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
static void parse_named_bind_decl(Parser *p);
static void parse_destructure_bind_tail(Parser *p, SyntaxKind bind_op,
                                          Checkpoint lhs_cp);
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
    // Statement-level bind LHS: `IDENT :: VALUE` / `IDENT := VALUE` /
    // `IDENT : TYPE`. Peek-ahead avoids emitting the IDENT as a bare
    // SK_REF_EXPR before recognizing the bind.
    if (precedence <= PREC_BIND && p_peek(p) == SK_IDENT) {
        SyntaxKind nx = p_peek_at(p, 1);
        if (nx == SK_COLON_COLON || nx == SK_COLON_EQ || nx == SK_COLON) {
            parse_named_bind_decl(p);
            return;
        }
    }

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
             tk == SK_CARET || tk == SK_QUESTION ||
             tk == SK_PLUS_PLUS || tk == SK_MINUS_MINUS ||
             (ore_kind_is_open_brace((OreSyntaxKind)tk) &&
              !p->parsing_type && !p->in_distinct_rhs))) {
            parse_infix(p, tk, left_cp);
            continue;
        }

        // Binary / assignment infix. Disabled in type position so e.g.
        // `fn() i32 -x` doesn't eat `i32 - x` as a BIN_SUB.
        if (p->parsing_type) break;
        Precedence prec = get_infix_precedence(tk);
        if (prec == PREC_NONE || (int)prec <= precedence) break;

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
    // `named` and `override` are contextual modifiers when followed
    // by handler/handle — punt to the handler parselet in that case.
    case SK_IDENT: {
        const Token *t = p_current(p);
        if ((TOK_IS(t, "named") || TOK_IS(t, "override"))) {
            SyntaxKind k1 = p_peek_at(p, 1);
            if (k1 == SK_HANDLER_KW || k1 == SK_HANDLE_KW) {
                parse_handler_expr(p);
                return;
            }
        }
        p_start_node(p, SK_REF_EXPR);
        p_advance(p);
        p_finish_node(p);
        return;
    }

    // ---- Grouping: (expr) — preserved as SK_PAREN_EXPR -----------
    case SK_LPAREN:
        p_start_node(p, SK_PAREN_EXPR);
        p_advance(p);  // (
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
        p_advance(p);  // forward progress
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
        p_advance(p);  // (
        while (!p_is_eof(p) && p_peek(p) != SK_RPAREN) {
            parse_expr(p, PREC_NONE);
            if (!p_match(p, SK_COMMA)) break;
        }
        p_consume(p, SK_RPAREN, "expected ')' after arguments");
        p_finish_node(p);  // SK_ARG_LIST
        p_finish_node(p);  // SK_CALL_EXPR
        return;
    }

    // ---- Postfix: index a[i] / slice a[lo..hi] --------------------
    if (op_kind == SK_LBRACKET) {
        // The decision between INDEX_EXPR and SLICE_EXPR happens after
        // parsing `lo`. Use a SK_INDEX_EXPR placeholder via checkpoint
        // and rewrap if it turns out to be a slice.
        Checkpoint cp = left_cp;  // base + the upcoming bracket form
        p_advance(p);  // [

        // `[..hi]` is rejected — Zig-strict. Consume the `..` for
        // recovery and treat as slice with NONE for lo.
        if (p_peek(p) == SK_DOT_DOT) {
            p_error(p, "open-left slice `[..hi]` not allowed; write `[0..hi]`");
            p_advance(p);  // ..
            if (p_peek(p) != SK_RBRACKET) parse_expr(p, PREC_NONE);
            p_consume(p, SK_RBRACKET, "expected ']' to close slice");
            p_start_node_at(p, cp, SK_SLICE_EXPR);
            p_finish_node(p);
            return;
        }

        parse_expr(p, PREC_NONE);  // lo

        if (p_match(p, SK_DOT_DOT)) {
            // slice
            if (p_peek(p) != SK_RBRACKET) parse_expr(p, PREC_NONE);
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
    if (op_kind == SK_DOT) {
        p_start_node_at(p, left_cp, SK_FIELD_EXPR);
        p_advance(p);  // .
        p_consume(p, SK_IDENT, "expected field name after '.'");
        p_finish_node(p);
        return;
    }

    // ---- Postfix unary: ^ ? ++ -- ---------------------------------
    if (op_kind == SK_CARET || op_kind == SK_QUESTION ||
        op_kind == SK_PLUS_PLUS || op_kind == SK_MINUS_MINUS) {
        p_start_node_at(p, left_cp, SK_POSTFIX_EXPR);
        p_advance(p);  // op token
        p_finish_node(p);
        return;
    }

    // ---- Typed construction T{...} or .Variant{...} ---------------
    if (ore_kind_is_open_brace((OreSyntaxKind)op_kind)) {
        p_start_node_at(p, left_cp, SK_PRODUCT_EXPR);
        // The type expr already sits at left_cp; emit the SK_INIT_LIST.
        p_start_node(p, SK_INIT_LIST);
        p_advance(p);  // {
        while (!p_is_eof(p) && !p_check(p, SK_RBRACE)) {
            uint32_t before = p->pos;
            p_start_node(p, SK_INIT_FIELD);
            // Optional `.name =` prefix for named fields.
            if (p_peek(p) == SK_DOT && p_peek_at(p, 1) == SK_IDENT) {
                p_advance(p);  // .
                p_advance(p);  // name ident
                p_consume(p, SK_EQ, "expected '=' after field name");
            }
            parse_expr(p, PREC_NONE);
            p_finish_node(p);  // SK_INIT_FIELD
            if (!p_match(p, SK_COMMA)) break;
            if (p->pos == before) p_advance(p);
        }
        while (p_match(p, SK_SEMI)) {} // layout `;` before `}`
        p_consume(p, SK_RBRACE, "expected '}' to close construction");
        p_finish_node(p);  // SK_INIT_LIST
        p_finish_node(p);  // SK_PRODUCT_EXPR
        return;
    }

    // ---- Bind ops (::, :=, :) -------------------------------------
    //
    // Named-bind LHS is handled in the parse_expr peek-ahead path.
    // The infix path only fires for destructure patterns (`.{...} ::`).
    if (op_kind == SK_COLON_COLON || op_kind == SK_COLON_EQ ||
        op_kind == SK_COLON) {
        parse_destructure_bind_tail(p, op_kind, left_cp);
        return;
    }

    // ---- Trailing-lambda `<-` -------------------------------------
    //
    // Emitted as a binary expression (SK_BIN_EXPR { lhs <- rhs }) so
    // the green tree mirrors the source. Sema desugars to a call-with-
    // trailing-lambda. The RHS is parsed as a lambda value: optional
    // `(params)` then a block.
    if (op_kind == SK_LARROW) {
        p_start_node_at(p, left_cp, SK_BIN_EXPR);
        p_advance(p);  // <-
        // RHS: optional `(params)` + block, packaged as a lambda
        // value for sema to extract.
        p_start_node(p, SK_LAMBDA_EXPR);
        if (p_match(p, SK_LPAREN)) {
            p_start_node(p, SK_PARAM_LIST);
            while (!p_is_eof(p) && p_peek(p) != SK_RPAREN) {
                parse_param(p, /*name_required=*/true);
                if (!p_match(p, SK_COMMA)) break;
            }
            p_consume(p, SK_RPAREN, "expected ')' to close lambda params");
            p_finish_node(p);
        }
        if (p_check(p, SK_LBRACE)) {
            parse_block(p);
        } else {
            p_error(p, "expected '{ ... }' (or an indented block) after '<-'");
        }
        p_finish_node(p);  // SK_LAMBDA_EXPR
        p_finish_node(p);  // SK_BIN_EXPR
        return;
    }

    // ---- Binary / assignment --------------------------------------
    Precedence prec = get_infix_precedence(op_kind);
    int next_prec = is_right_associative(op_kind) ? (int)prec - 1 : (int)prec;

    SyntaxKind wrap = ore_kind_is_assign_op_token((OreSyntaxKind)op_kind)
                          ? SK_ASSIGN_EXPR
                          : SK_BIN_EXPR;

    p_start_node_at(p, left_cp, wrap);
    p_advance(p);  // op token
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

static void parse_named_bind_decl(Parser *p) {
    // LHS = a bare IDENT. We retroactively wrap it in SK_CONST_DECL/
    // SK_VAR_DECL via a checkpoint.
    Checkpoint cp = p_checkpoint(p);
    p_advance(p);  // IDENT — emitted to the green tree as a child
    SyntaxKind bind_op = p_peek(p);
    emit_bind_decl_tail(p, bind_op, cp, /*is_destructure=*/false);
}

static void parse_destructure_bind_tail(Parser *p, SyntaxKind bind_op,
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
    if (!t) return false;
    if (t->kind == SK_COMPTIME_KW) return true;
    if (t->kind != SK_IDENT) return false;
    return TOK_IS(t, "pub") || TOK_IS(t, "pvt") || TOK_IS(t, "abstract") ||
           TOK_IS(t, "named") || TOK_IS(t, "scoped") || TOK_IS(t, "linear") ||
           TOK_IS(t, "distinct");
}

static void emit_bind_decl_tail(Parser *p, SyntaxKind bind_op,
                                  Checkpoint lhs_cp, bool is_destructure) {
    bool is_typed = (bind_op == SK_COLON);
    bool is_const = (bind_op == SK_COLON_COLON);
    SyntaxKind wrap;
    if (is_destructure)      wrap = SK_DESTRUCTURE_DECL;
    else if (is_const)       wrap = SK_CONST_DECL;
    else                     wrap = SK_VAR_DECL;

    p_start_node_at(p, lhs_cp, wrap);
    p_advance(p);  // bind op token (::, :=, or :)

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
    p_finish_node(p);  // SK_*_DECL
}


// ---------------------------------------------------------------------
// Helpers: optional label, single parameter.
// ---------------------------------------------------------------------

static void parse_optional_label(Parser *p) {
    if (!p_match(p, SK_COLON)) return;
    p_consume(p, SK_IDENT, "expected label name after ':'");
}

static void parse_param(Parser *p, bool name_required) {
    p_start_node(p, SK_PARAM);
    p_match(p, SK_COMPTIME_KW);
    if (name_required) {
        if (p_consume(p, SK_IDENT, "expected parameter name")) {
            if (p_match(p, SK_COLON)) parse_type_expr(p);
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
    p_advance(p);  // op token
    parse_expr(p, PREC_UNARY);
    p_finish_node(p);
}

static void parse_return_expr(Parser *p) {
    p_start_node(p, SK_RETURN_STMT);
    p_advance(p);  // return
    OreSyntaxKind nx = (OreSyntaxKind)p_peek(p);
    if (!ore_kind_is_stmt_sep(nx) && !ore_kind_is_close_brace(nx) &&
        nx != SK_EOF) {
        parse_expr(p, PREC_NONE);
    }
    p_finish_node(p);
}

static void parse_defer_expr(Parser *p) {
    p_start_node(p, SK_DEFER_STMT);
    p_advance(p);  // defer
    parse_expr(p, PREC_NONE);
    p_finish_node(p);
}

static void parse_break_or_continue(Parser *p, SyntaxKind wrap_kind) {
    p_start_node(p, wrap_kind);
    p_advance(p);  // break / continue
    parse_optional_label(p);
    p_finish_node(p);
}


// ---------------------------------------------------------------------
// Control flow: if, loop, switch.
// ---------------------------------------------------------------------

static void parse_if_expr(Parser *p) {
    p_start_node(p, SK_IF_EXPR);
    p_advance(p);  // if / elif
    p_consume(p, SK_LPAREN, "expected '(' after if");
    parse_expr(p, PREC_NONE);
    p_consume(p, SK_RPAREN, "expected ')' after if condition");
    parse_expr(p, PREC_NONE);  // then branch
    if (p_peek(p) == SK_ELIF_KW) {
        parse_if_expr(p);  // chained as nested if
    } else if (p_match(p, SK_ELSE_KW)) {
        parse_expr(p, PREC_NONE);
    }
    p_finish_node(p);
}

static void parse_loop_expr(Parser *p) {
    p_start_node(p, SK_LOOP_EXPR);
    p_advance(p);  // loop
    parse_optional_label(p);
    if (p_match(p, SK_LPAREN)) {
        // `loop (cond)` — while; or `loop (init; cond; step)` — C-for.
        parse_expr(p, PREC_NONE);
        if (p_match(p, SK_SEMI)) {
            parse_expr(p, PREC_NONE);  // cond
            p_consume(p, SK_SEMI, "expected ';' in for-style loop");
            parse_expr(p, PREC_NONE);  // step
        }
        p_consume(p, SK_RPAREN, "expected ')' after loop header");
    }
    parse_expr(p, PREC_NONE);  // body
    p_finish_node(p);
}

static void parse_switch_expr(Parser *p) {
    p_start_node(p, SK_MATCH_EXPR);
    p_advance(p);  // switch
    p_consume(p, SK_LPAREN, "expected '(' after switch");
    parse_expr(p, PREC_NONE);  // scrutinee
    p_consume(p, SK_RPAREN, "expected ')' after switch scrutinee");
    p_consume(p, SK_LBRACE, "expected '{' to open switch body");
    p_start_node(p, SK_STMT_LIST);
    while (!p_is_eof(p) && !p_check(p, SK_RBRACE)) {
        uint32_t before = p->pos;
        p_start_node(p, SK_SWITCH_ARM);
        // Pattern alternatives separated by `|`.
        for (;;) {
            parse_expr(p, PREC_OR + 1);  // stop before `|`
            if (!p_match(p, SK_PIPE)) break;
        }
        p_consume(p, SK_FATARROW, "expected '=>' in switch arm");
        parse_expr(p, PREC_NONE);  // body
        p_finish_node(p);  // SK_SWITCH_ARM
        p_match(p, SK_SEMI);
        if (p->pos == before) p_advance(p);
    }
    p_finish_node(p);  // SK_STMT_LIST
    p_consume(p, SK_RBRACE, "expected '}' to close switch");
    p_finish_node(p);  // SK_MATCH_EXPR
}


// ---------------------------------------------------------------------
// fn-lambda and Fn-type.
// ---------------------------------------------------------------------

static void parse_fn_lambda(Parser *p) {
    p_start_node(p, SK_LAMBDA_EXPR);
    p_advance(p);  // fn
    p_consume(p, SK_LPAREN, "expected '(' after fn");
    p_start_node(p, SK_PARAM_LIST);
    while (!p_is_eof(p) && p_peek(p) != SK_RPAREN) {
        parse_param(p, /*name_required=*/true);
        if (!p_match(p, SK_COMMA)) break;
    }
    p_consume(p, SK_RPAREN, "expected ')' after parameters");
    p_finish_node(p);  // SK_PARAM_LIST

    // Effect row `<...>` (only legal directly after `)`).
    if (p_peek(p) == SK_LT) parse_effect_row(p);

    // Bare return type (no `->`). Locked rule: body is always `{ }`.
    // The return type is gated by terminator tokens.
    OreSyntaxKind rk = (OreSyntaxKind)p_peek(p);
    if (!ore_kind_is_open_brace(rk) && rk != SK_RPAREN && rk != SK_COMMA &&
        rk != SK_GT && !ore_kind_is_stmt_sep(rk) &&
        !ore_kind_is_close_brace(rk) && rk != SK_PIPE && rk != SK_EOF) {
        parse_type_expr(p);
    }

    // Body — `{ ... }` (explicit or virtual). No body => fn-type form.
    if (p_check(p, SK_LBRACE)) {
        parse_block(p);
    }
    p_finish_node(p);  // SK_LAMBDA_EXPR
}

static void parse_fn_type(Parser *p) {
    p_start_node(p, SK_FN_TYPE);
    p_advance(p);  // Fn
    p_consume(p, SK_LPAREN, "expected '(' after Fn");
    p_start_node(p, SK_PARAM_LIST);
    while (!p_is_eof(p) && p_peek(p) != SK_RPAREN) {
        parse_param(p, /*name_required=*/false);
        if (!p_match(p, SK_COMMA)) break;
    }
    p_consume(p, SK_RPAREN, "expected ')'");
    p_finish_node(p);  // SK_PARAM_LIST

    if (p_peek(p) == SK_LT) parse_effect_row(p);

    OreSyntaxKind rk = (OreSyntaxKind)p_peek(p);
    if (rk != SK_RPAREN && rk != SK_COMMA && rk != SK_GT &&
        !ore_kind_is_stmt_sep(rk) && !ore_kind_is_close_brace(rk) &&
        rk != SK_RBRACKET && rk != SK_PIPE && !ore_kind_is_open_brace(rk) &&
        rk != SK_EOF) {
        parse_type_expr(p);
    }
    p_finish_node(p);  // SK_FN_TYPE
}


// ---------------------------------------------------------------------
// Aggregates: struct / union / enum.
// ---------------------------------------------------------------------

static void parse_aggregate_expr(Parser *p, SyntaxKind kind, bool consume_kw) {
    p_start_node(p, kind);
    if (consume_kw) p_advance(p);  // struct/union keyword
    p_consume(p, SK_LBRACE, "expected '{' after struct/union");
    p_start_node(p, SK_FIELD_LIST);
    while (!p_is_eof(p) && !p_check(p, SK_RBRACE)) {
        uint32_t before = p->pos;

        // Optional `pub` modifier (contextual ident).
        if (p_peek(p) == SK_IDENT && TOK_IS(p_current(p), "pub")) {
            p_advance(p);
        }

        // Anonymous nested aggregate (struct { union { ... } }).
        if (p_peek(p) == SK_STRUCT_KW || p_peek(p) == SK_UNION_KW ||
            p_peek(p) == SK_ENUM_KW) {
            p_start_node(p, SK_FIELD);
            parse_type_expr(p);
            p_finish_node(p);
            if (!p_match(p, SK_COMMA)) p_match(p, SK_SEMI);
            if (p->pos == before) p_advance(p);
            continue;
        }

        p_start_node(p, SK_FIELD);
        if (!p_consume(p, SK_IDENT, "expected field name")) break;
        p_consume(p, SK_COLON, "expected ':' after field name");
        parse_type_expr(p);
        // Optional `= <const>` for explicit offset / default.
        if (p_match(p, SK_EQ)) parse_expr(p, PREC_BITWISE);
        p_finish_node(p);  // SK_FIELD

        if (!p_match(p, SK_COMMA)) p_match(p, SK_SEMI);
        if (p->pos == before) p_advance(p);
    }
    p_finish_node(p);  // SK_FIELD_LIST
    p_consume(p, SK_RBRACE, "expected '}' to close struct/union");
    p_finish_node(p);  // SK_STRUCT_DECL / SK_UNION_DECL
}

static void parse_enum_expr(Parser *p) {
    p_start_node(p, SK_ENUM_DECL);
    p_advance(p);  // enum
    p_consume(p, SK_LBRACE, "expected '{' after enum");
    p_start_node(p, SK_VARIANT_LIST);
    while (!p_is_eof(p) && !p_check(p, SK_RBRACE)) {
        uint32_t before = p->pos;
        p_start_node(p, SK_VARIANT);
        if (!p_consume(p, SK_IDENT, "expected variant name")) {
            p_finish_node(p);
            break;
        }
        if (p_match(p, SK_EQ)) parse_expr(p, PREC_BITWISE);
        p_finish_node(p);  // SK_VARIANT
        if (!p_match(p, SK_COMMA)) p_match(p, SK_SEMI);
        if (p->pos == before) p_advance(p);
    }
    p_finish_node(p);  // SK_VARIANT_LIST
    p_consume(p, SK_RBRACE, "expected '}' to close enum");
    p_finish_node(p);  // SK_ENUM_DECL
}


// ---------------------------------------------------------------------
// Bracket forms — slice/array/many-pointer/inferred-array.
// ---------------------------------------------------------------------

static void parse_bracket_expr(Parser *p) {
    // [^]T — many-pointer
    if (p_peek_at(p, 1) == SK_CARET) {
        p_start_node(p, SK_MANY_PTR_TYPE);
        p_advance(p);  // [
        p_advance(p);  // ^
        p_consume(p, SK_RBRACKET, "expected ']' after '[^'");
        parse_type_expr(p);
        p_finish_node(p);
        return;
    }
    // []T — slice
    if (p_peek_at(p, 1) == SK_RBRACKET) {
        p_start_node(p, SK_SLICE_TYPE);
        p_advance(p);  // [
        p_advance(p);  // ]
        parse_type_expr(p);
        p_finish_node(p);
        return;
    }
    // [_]T — inferred-size array
    if (p_peek_at(p, 1) == SK_UNDERSCORE && p_peek_at(p, 2) == SK_RBRACKET) {
        p_start_node(p, SK_ARRAY_TYPE);
        p_advance(p);  // [
        p_advance(p);  // _
        p_advance(p);  // ]
        parse_type_expr(p);
        p_finish_node(p);
        return;
    }
    // [N]T — sized array
    p_start_node(p, SK_ARRAY_TYPE);
    p_advance(p);  // [
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
        p_advance(p);  // .
        p_start_node(p, SK_INIT_LIST);
        p_advance(p);  // {
        while (!p_is_eof(p) && !p_check(p, SK_RBRACE)) {
            uint32_t before = p->pos;
            p_start_node(p, SK_INIT_FIELD);
            if (p_peek(p) == SK_DOT && p_peek_at(p, 1) == SK_IDENT) {
                p_advance(p);  // .
                p_advance(p);  // name
                p_consume(p, SK_EQ, "expected '=' after field name");
            }
            parse_expr(p, PREC_NONE);
            p_finish_node(p);  // SK_INIT_FIELD
            if (!p_match(p, SK_COMMA)) break;
            if (p->pos == before) p_advance(p);
        }
        while (p_match(p, SK_SEMI)) {}
        p_consume(p, SK_RBRACE, "expected '}' to close product literal");
        p_finish_node(p);  // SK_INIT_LIST
        p_finish_node(p);  // SK_PRODUCT_EXPR
        return;
    }
    if (next == SK_IDENT) {
        p_start_node(p, SK_ENUM_REF_EXPR);
        p_advance(p);  // .
        p_advance(p);  // ident
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
    p_advance(p);  // @
    if (!p_consume(p, SK_IDENT, "expected builtin name after '@'")) {
        p_finish_node(p);
        return;
    }
    if (p_match(p, SK_LPAREN)) {
        p_start_node(p, SK_ARG_LIST);
        while (!p_is_eof(p) && p_peek(p) != SK_RPAREN) {
            parse_expr(p, PREC_NONE);
            if (!p_match(p, SK_COMMA)) break;
        }
        p_consume(p, SK_RPAREN, "expected ')' after builtin args");
        p_finish_node(p);  // SK_ARG_LIST
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
        if (!p_match(p, SK_COMMA)) break;
    }
    p_consume(p, SK_GT, "expected '>' to close effect row");
    p_finish_node(p);
}

// Parse a handler clause body. Returns true if a clause was recognized.
static bool parse_handler_clause(Parser *p) {
    uint32_t entry = p->pos;
    SyntaxKind k = p_peek(p);

    if (k == SK_RETURN_KW) {
        p_start_node(p, SK_LAMBDA_EXPR);
        p_advance(p);  // return
        if (p_match(p, SK_LPAREN)) {
            p_start_node(p, SK_PARAM_LIST);
            while (!p_is_eof(p) && p_peek(p) != SK_RPAREN) {
                parse_param(p, /*name_required=*/true);
                if (!p_match(p, SK_COMMA)) break;
            }
            p_consume(p, SK_RPAREN, "expected ')' after return params");
            p_finish_node(p);
        }
        parse_expr(p, PREC_NONE);
        p_finish_node(p);
        return true;
    }

    if (k == SK_IDENT) {
        const Token *t = p_current(p);
        if (TOK_IS(t, "initially") || TOK_IS(t, "finally")) {
            p_start_node(p, SK_LAMBDA_EXPR);
            p_advance(p);  // initially / finally
            parse_expr(p, PREC_NONE);
            p_finish_node(p);
            return true;
        }
        // Op clause: Name :: [pub] [final|raw] (fn|ctl|val) (params) body
        if (p_peek_at(p, 1) == SK_COLON_COLON) {
            p_start_node(p, SK_FIELD);  // reuse SK_FIELD as the op-clause wrapper
            p_advance(p);  // name
            p_advance(p);  // ::
            if (p_peek(p) == SK_IDENT && TOK_IS(p_current(p), "pub"))
                p_advance(p);
            // op-kind keyword(s)
            if (p_peek(p) == SK_FN_KW) {
                p_advance(p);
            } else if (p_peek(p) == SK_IDENT) {
                const Token *kt = p_current(p);
                if (TOK_IS(kt, "ctl") || TOK_IS(kt, "val")) {
                    p_advance(p);
                } else if ((TOK_IS(kt, "final") || TOK_IS(kt, "raw")) &&
                           p_peek_at(p, 1) == SK_IDENT) {
                    const Token *kt2 = (const Token *)p->tokens->data + p->pos + 1;
                    if (TOK_IS(kt2, "ctl")) {
                        p_advance(p);
                        p_advance(p);
                    }
                }
            }
            // val: `name :: val :: value`
            if (p_match(p, SK_COLON_COLON)) {
                parse_expr(p, PREC_BIND);
            } else {
                // op signature: optional (params), then body
                if (p_match(p, SK_LPAREN)) {
                    p_start_node(p, SK_PARAM_LIST);
                    while (!p_is_eof(p) && p_peek(p) != SK_RPAREN) {
                        parse_param(p, /*name_required=*/true);
                        if (!p_match(p, SK_COMMA)) break;
                    }
                    p_consume(p, SK_RPAREN, "expected ')' after op params");
                    p_finish_node(p);
                }
                parse_expr(p, PREC_NONE);
            }
            p_finish_node(p);  // op-clause SK_FIELD
            return true;
        }
    }

    p->pos = entry;
    return false;
}

static void parse_handler_expr(Parser *p) {
    p_start_node(p, SK_HANDLER_EXPR);

    // Optional `named` / `override` modifier already consumed by the
    // dispatcher (parse_prefix); reach this either right at `handler` /
    // `handle` or one token past the modifier. The modifier was already
    // emitted as an ident token by p_advance.
    bool is_handle = false;
    if (p_peek(p) == SK_HANDLE_KW) {
        is_handle = true;
        p_advance(p);
    } else if (p_peek(p) == SK_HANDLER_KW) {
        p_advance(p);
    } else {
        p_error(p, "expected 'handler' or 'handle'");
    }

    // Optional `scoped` contextual modifier.
    if (p_peek(p) == SK_IDENT && TOK_IS(p_current(p), "scoped"))
        p_advance(p);

    // Optional effect row.
    if (p_peek(p) == SK_LT) parse_effect_row(p);

    // `handle` form: `( action )` between effect row and body.
    if (is_handle) {
        p_consume(p, SK_LPAREN, "expected '(' after 'handle'");
        parse_expr(p, PREC_NONE);
        p_consume(p, SK_RPAREN, "expected ')' after handle action");
    }

    // Body: `{ clauses... }`.
    p_consume(p, SK_LBRACE, "expected '{' to start handler body");
    p_start_node(p, SK_FIELD_LIST);
    while (!p_is_eof(p) && !p_check(p, SK_RBRACE)) {
        uint32_t before = p->pos;
        if (parse_handler_clause(p)) {
            p_consume(p, SK_SEMI, "expected ';' after handler clause");
        } else {
            p_error(p, "expected a handler operation or return/initially/finally");
            if (p->pos == before) p_advance(p);
        }
    }
    p_finish_node(p);  // SK_FIELD_LIST
    p_consume(p, SK_RBRACE, "expected '}' to end handler body");

    p_finish_node(p);  // SK_HANDLER_EXPR

    // Note: `handle` form should logically wrap the handler+action in
    // SK_HANDLE_EXPR. For now we emit a flat SK_HANDLER_EXPR with the
    // action as part of its prefix; sema can re-split. A future refinement
    // will move the prefix-emit ordering to nest correctly.
}

static void parse_mask_expr(Parser *p) {
    p_start_node(p, SK_MASK_EXPR);
    p_advance(p);  // mask
    if (p_peek(p) == SK_IDENT && TOK_IS(p_current(p), "behind"))
        p_advance(p);
    parse_effect_row(p);
    parse_expr(p, PREC_NONE);
    p_finish_node(p);
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
    p_advance(p);  // with

    // Optional `binder :=` (name only, no type).
    if (p_peek(p) == SK_IDENT && p_peek_at(p, 1) == SK_COLON_EQ) {
        parse_param(p, /*name_required=*/true);
        p_consume(p, SK_COLON_EQ, "expected ':=' after with-binder");
    }

    // Head expression.
    parse_expr(p, PREC_NONE);

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
            if (p->pos == before) p_advance(p);
        }
        p_finish_node(p);  // SK_STMT_LIST
        p_finish_node(p);  // SK_BLOCK_STMT
    }
    p_finish_node(p);  // SK_HANDLE_EXPR (the with-statement carrier)
}

static void parse_effect_decl(Parser *p) {
    p_start_node(p, SK_EFFECT_DECL);
    p_advance(p);  // effect

    // Optional `< T1, T2 >` type parameters.
    if (p_match(p, SK_LT)) {
        while (!p_is_eof(p) && p_peek(p) != SK_GT) {
            parse_type_expr(p);
            if (!p_match(p, SK_COMMA)) break;
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
                const Token *kt2 = (const Token *)p->tokens->data + p->pos + 1;
                if (TOK_IS(kt2, "ctl")) {
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
                if (!p_match(p, SK_COMMA)) break;
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
        p_finish_node(p);  // SK_FIELD
        p_consume(p, SK_SEMI, "expected ';' after operation signature");
        if (p->pos == before) p_advance(p);
    }
    p_finish_node(p);  // SK_FIELD_LIST
    p_consume(p, SK_RBRACE, "expected '}' to end effect body");
    p_finish_node(p);  // SK_EFFECT_DECL
}
