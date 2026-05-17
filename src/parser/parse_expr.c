#include "parse_expr.h"
#include "parse_stmt.h"

// =============================================================================
// Precedence Table
// =============================================================================
//
// Mirrors the Ore precedence ladder documented in src/parser/GRAMMAR.md §2.3.
// PREC_POSTFIX tokens (LPAREN call, DOT field, LBRACKET index/slice, postfix
// CARET / QUESTION / BANG / PLUS_PLUS) are NOT in this table — postfix
// handling lives in `parse_infix` and runs unconditionally before the binary
// precedence comparison.

static Precedence get_infix_precedence(TokenKind kind) {
  switch (kind) {
  case TK_COLON_COLON:
  case TK_COLON_EQ:
  case TK_COLON:
    return PREC_BIND;

  // `<-` is the trailing-lambda / continuation operator (`callee <- (p)
  // { body }`, and `with p <- expr`). `=>` is NOT an infix — it is only
  // the switch arm separator, consumed directly by parse_switch_expr.
  case TK_LARROW:
    return PREC_LAMBDA;

  case TK_EQ:
  case TK_PLUS_EQ:
  case TK_MINUS_EQ:
  case TK_STAR_EQ:
  case TK_SLASH_EQ:
  case TK_PERCENT_EQ:
  case TK_AMP_EQ:
  case TK_PIPE_EQ:
  case TK_CARET_EQ:
    return PREC_ASSIGN;

  case TK_PIPE_PIPE:
  case TK_ORELSE:
    return PREC_OR;

  case TK_AMP_AMP:
    return PREC_AND;

  case TK_EQ_EQ:
  case TK_BANG_EQ:
    return PREC_EQUALITY;

  case TK_LT:
  case TK_LE:
  case TK_GT:
  case TK_GE:
    return PREC_COMPARISON;

  case TK_PIPE:
  case TK_AMP:
  case TK_CARET:
    return PREC_BITWISE;

  case TK_SHL:
  case TK_SHR:
    return PREC_SHIFT;

  case TK_PLUS:
  case TK_MINUS:
    return PREC_TERM;

  case TK_STAR:
  case TK_SLASH:
  case TK_PERCENT:
    return PREC_FACTOR;

  case TK_STAR_STAR:
    return PREC_POWER;

  default:
    return PREC_NONE;
  }
}

// `**` and the assignment family are right-associative; everything else is
// left-associative.
static bool is_right_associative(TokenKind kind) {
  switch (kind) {
  case TK_STAR_STAR:
  case TK_EQ:
  case TK_PLUS_EQ:
  case TK_MINUS_EQ:
  case TK_STAR_EQ:
  case TK_SLASH_EQ:
  case TK_PERCENT_EQ:
  case TK_AMP_EQ:
  case TK_PIPE_EQ:
  case TK_CARET_EQ:
    return true;
  default:
    return false;
  }
}

static AstNodeKind get_binary_op_kind(TokenKind kind) {
  switch (kind) {
  case TK_PLUS:
    return AST_EXPR_BIN_ADD;
  case TK_MINUS:
    return AST_EXPR_BIN_SUB;
  case TK_STAR:
    return AST_EXPR_BIN_MUL;
  case TK_SLASH:
    return AST_EXPR_BIN_DIV;
  case TK_PERCENT:
    return AST_EXPR_BIN_MOD;
  case TK_STAR_STAR:
    return AST_EXPR_BIN_POW;
  case TK_EQ_EQ:
    return AST_EXPR_BIN_EQ;
  case TK_BANG_EQ:
    return AST_EXPR_BIN_NEQ;
  case TK_LT:
    return AST_EXPR_BIN_LT;
  case TK_LE:
    return AST_EXPR_BIN_LE;
  case TK_GT:
    return AST_EXPR_BIN_GT;
  case TK_GE:
    return AST_EXPR_BIN_GE;
  case TK_AMP_AMP:
    return AST_EXPR_BIN_AND;
  case TK_PIPE_PIPE:
    return AST_EXPR_BIN_OR;
  case TK_ORELSE:
    return AST_EXPR_BIN_ORELSE;
  case TK_AMP:
    return AST_EXPR_BIN_BIT_AND;
  case TK_PIPE:
    return AST_EXPR_BIN_BIT_OR;
  case TK_CARET:
    return AST_EXPR_BIN_BIT_XOR;
  case TK_SHL:
    return AST_EXPR_BIN_SHL;
  case TK_SHR:
    return AST_EXPR_BIN_SHR;
  case TK_EQ:
    return AST_EXPR_ASSIGN;
  case TK_PLUS_EQ:
    return AST_EXPR_ASSIGN_ADD;
  case TK_MINUS_EQ:
    return AST_EXPR_ASSIGN_SUB;
  case TK_STAR_EQ:
    return AST_EXPR_ASSIGN_MUL;
  case TK_SLASH_EQ:
    return AST_EXPR_ASSIGN_DIV;
  case TK_PERCENT_EQ:
    return AST_EXPR_ASSIGN_MOD;
  case TK_AMP_EQ:
    return AST_EXPR_ASSIGN_BIT_AND;
  case TK_PIPE_EQ:
    return AST_EXPR_ASSIGN_BIT_OR;
  case TK_CARET_EQ:
    return AST_EXPR_ASSIGN_BIT_XOR;
  default:
    return AST_ERROR;
  }
}

// =============================================================================
// Span / push helpers
// =============================================================================

// Last consumed token. Used by helpers that need the end-of-form token to
// build the full span. Safe because the parser always advances past at least
// one token before any helper returns.
static inline const Token *p_prev(Parser *p) {
  return vec_get((Vec *)p->tokens, p->pos - 1);
}

static inline TinySpan p_node_span(const Parser *p, AstNodeId id) {
  return *(TinySpan *)vec_get(&p->span_map, id.idx);
}

static inline TinySpan span_from_to(const Parser *p, const Token *start,
                                    const Token *end) {
  return span_make_range((uint16_t)p->file.idx, start->start, end->byte_end);
}

// =============================================================================
// Forward decls
// =============================================================================

static AstNodeId parse_prefix(Parser *p);
static AstNodeId parse_infix(Parser *p, AstNodeId left, TinySpan left_span);

// Trailing-lambda desugar, shared by the `<-` infix parselet and the
// `with` statement: append `lambda` as the trailing argument of `left`
// (rebuilding its arg run if `left` is already a call), else wrap
// `left(lambda)`. Produces one AST_EXPR_CALL spanning `span`.
static AstNodeId emit_trailing_call(Parser *p, AstNodeId left,
                                    AstNodeId lambda, uint32_t op_index,
                                    TinySpan span);
// parse_type_expr is exported (parse_expr.h) — used by parse_decl's typed
// binds.

// =============================================================================
// Helpers — simple ident / wildcard / synthetic ident node from a Token
// =============================================================================

static AstNodeId emit_ident(Parser *p, const Token *tok, AstNodeKind kind) {
  AstNodeData d = {0};
  d.string_id = tok->string_id;
  return p_push_node(p, kind, p->pos - 1, d, p_span(p, tok, tok));
}

// =============================================================================
// Parameter parser — shared by fn-lambda and Fn-type.
//
// Forms:
//   `[comptime] name [: T]`           — for fn(...) value-position lambda
//   `T`                                — for Fn(T1, T2) type-position
//
// Extras layout for AST_DECL_VAL param node: [name_id, type_id, is_comptime].
// name_id is AST_NODE_ID_NONE for type-only params (Fn-type case).
// =============================================================================

static AstNodeId parse_param(Parser *p, bool name_required) {
  const Token *first_tok = p_current(p);
  uint32_t op_index = p->pos;

  bool is_comptime = p_match(p, TK_COMPTIME);
  AstNodeId name_id = AST_NODE_ID_NONE;
  AstNodeId type_id = AST_NODE_ID_NONE;

  if (name_required) {
    const Token *name_tok =
        p_consume(p, TK_IDENTIFIER, "Expected parameter name");
    if (!name_tok)
      return AST_NODE_ID_NONE;
    AstNodeData nd = {0};
    nd.string_id = name_tok->string_id;
    name_id = p_push_node(p, AST_EXPR_PATH, p->pos - 1, nd,
                          p_span(p, name_tok, name_tok));

    if (p_match(p, TK_COLON)) {
      type_id = parse_type_expr(p);
    }
  } else {
    // Type-only param: the whole thing is one type expression.
    type_id = parse_type_expr(p);
  }

  uint32_t payload[3] = {name_id.idx, type_id.idx, is_comptime ? 1u : 0u};
  AstExtraDataIdx extra = ast_push_extra(p->ast, payload, 3);
  AstNodeData data = {0};
  data.extra_idx = extra;

  const Token *end_tok = p_prev(p);
  return p_push_node(p, AST_DECL_VAL, op_index, data,
                     span_from_to(p, first_tok, end_tok));
}

// =============================================================================
// fn(params) <effects?> -> ret_type? body? — lambda literal / fn decl RHS
//
// Extras layout (AST_EXPR_LAMBDA):
//   [ret_type_id, body_id, effect_id, param_count, param0, param1, ...]
// =============================================================================

static AstNodeId parse_fn_lambda(Parser *p) {
  const Token *start_tok = p_advance(p); // consume TK_FN
  uint32_t op_index = p->pos - 1;

  if (!p_consume(p, TK_LPAREN, "Expected '(' after fn"))
    return AST_NODE_ID_NONE;

  // extras = [ret, body, effect, param_count, params...]. Reserve the
  // 4 header slots; they're backpatched after ret/body are parsed.
  uint32_t st = scratch_open(p);
  uint32_t h_ret = scratch_reserve(p);
  uint32_t h_body = scratch_reserve(p);
  uint32_t h_eff = scratch_reserve(p);
  uint32_t h_pc = scratch_reserve(p);

  uint32_t param_count = 0;
  if (!p_match(p, TK_VOID)) { // `fn(void)` is a zero-param lambda
    while (!p_is_eof(p) && p_peek(p) != TK_RPAREN) {
      AstNodeId param = parse_param(p, /*name_required=*/true);
      if (param.idx) {
        scratch_push(p, param.idx);
        param_count++;
      }
      if (!p_match(p, TK_COMMA))
        break;
    }
  }
  p_consume(p, TK_RPAREN, "Expected ')' after parameters");

  // Optional effect annotation `<E>` / `<E|e>` / `<|e>`. Defer the
  // full parse to follow-up — accept it as opaque and ignore for now.
  // The `<` here is unambiguous because it directly follows `)` of a
  // fn signature; outside this slot, `<` is comparison.
  AstNodeId effect_id = AST_NODE_ID_NONE;
  if (p_match(p, TK_LT)) {
    // Skip until matching `>`. TODO: real effect-row parse.
    int depth = 1;
    while (!p_is_eof(p) && depth > 0) {
      TokenKind tk = p_peek(p);
      if (tk == TK_LT)
        depth++;
      else if (tk == TK_GT)
        depth--;
      if (depth > 0)
        p_advance(p);
    }
    p_match(p, TK_GT);
  }

  // Optional `-> T` return type, gated by an explicit arrow.
  AstNodeId ret_type = AST_NODE_ID_NONE;
  if (p_match(p, TK_RARROW)) {
    ret_type = parse_type_expr(p);
  }

  // Optional body. A body-less fn is a function-type / forward decl.
  // Tokens that can never start a body: closing delimiters, comma, semi.
  AstNodeId body = AST_NODE_ID_NONE;
  TokenKind nx = p_peek(p);
  if (nx != TK_RPAREN && nx != TK_COMMA && nx != TK_GT && nx != TK_SEMI &&
      nx != TK_RBRACE && nx != TK_PIPE && nx != TK_EOF) {
    body = parse_expr(p, PREC_NONE);
  }

  scratch_set(p, h_ret, ret_type.idx);
  scratch_set(p, h_body, body.idx);
  scratch_set(p, h_eff, effect_id.idx);
  scratch_set(p, h_pc, param_count);
  AstExtraDataIdx extra = scratch_emit(p, st);
  AstNodeData data = {0};
  data.extra_idx = extra;

  const Token *end_tok = p_prev(p);
  return p_push_node(p, AST_EXPR_LAMBDA, op_index, data,
                     span_from_to(p, start_tok, end_tok));
}

// =============================================================================
// Fn(T1, T2) -> R — type-position function constructor
//
// Extras layout (AST_TYPE_FN): [ret_type_id, param_count, param0, ...]
// =============================================================================

static AstNodeId parse_fn_type(Parser *p) {
  const Token *start_tok = p_advance(p); // consume TK_FN_TYPE
  uint32_t op_index = p->pos - 1;

  if (!p_consume(p, TK_LPAREN, "Expected '(' after Fn"))
    return AST_NODE_ID_NONE;

  // extras = [ret, param_count, params...]; header slots backpatched.
  uint32_t st = scratch_open(p);
  uint32_t h_ret = scratch_reserve(p);
  uint32_t h_pc = scratch_reserve(p);

  uint32_t param_count = 0;
  while (!p_is_eof(p) && p_peek(p) != TK_RPAREN) {
    AstNodeId t = parse_param(p, /*name_required=*/false);
    if (t.idx) {
      scratch_push(p, t.idx);
      param_count++;
    }
    if (!p_match(p, TK_COMMA))
      break;
  }
  p_consume(p, TK_RPAREN, "Expected ')'");

  AstNodeId ret_type = AST_NODE_ID_NONE;
  if (p_match(p, TK_RARROW)) {
    ret_type = parse_type_expr(p);
  }

  scratch_set(p, h_ret, ret_type.idx);
  scratch_set(p, h_pc, param_count);
  AstExtraDataIdx extra = scratch_emit(p, st);
  AstNodeData data = {0};
  data.extra_idx = extra;

  return p_push_node(p, AST_TYPE_FN, op_index, data,
                     span_from_to(p, start_tok, p_prev(p)));
}

// =============================================================================
// if (cond) then [else else_branch]   — `elif` is part of the chain
//
// Extras layout (AST_STMT_IF): [cond_id, then_id, else_id]
// =============================================================================

static AstNodeId parse_if_expr(Parser *p) {
  const Token *start_tok = p_advance(p); // consume TK_IF or TK_ELIF
  uint32_t op_index = p->pos - 1;

  p_consume(p, TK_LPAREN, "Expected '(' after if");
  AstNodeId cond = parse_expr(p, PREC_NONE);
  p_consume(p, TK_RPAREN, "Expected ')' after if condition");

  AstNodeId then_branch = parse_expr(p, PREC_NONE);
  AstNodeId else_branch = AST_NODE_ID_NONE;

  if (p_peek(p) == TK_ELIF) {
    else_branch = parse_if_expr(p); // chains as nested if
  } else if (p_match(p, TK_ELSE)) {
    else_branch = parse_expr(p, PREC_NONE);
  }

  uint32_t payload[3] = {cond.idx, then_branch.idx, else_branch.idx};
  AstExtraDataIdx extra = ast_push_extra(p->ast, payload, 3);
  AstNodeData data = {0};
  data.extra_idx = extra;

  return p_push_node(p, AST_STMT_IF, op_index, data,
                     span_from_to(p, start_tok, p_prev(p)));
}

// =============================================================================
// loop body                 — infinite
// loop (cond) body          — while
// loop (init; cond; step) body — C-style for
//
// Extras layout (AST_STMT_LOOP): [init_id, cond_id, step_id, body_id]
// =============================================================================

static AstNodeId parse_loop_expr(Parser *p) {
  const Token *start_tok = p_advance(p); // consume TK_LOOP
  uint32_t op_index = p->pos - 1;

  AstNodeId init = AST_NODE_ID_NONE;
  AstNodeId cond = AST_NODE_ID_NONE;
  AstNodeId step = AST_NODE_ID_NONE;

  if (p_match(p, TK_LPAREN)) {
    AstNodeId first = parse_expr(p, PREC_NONE);
    // C-style: `loop (init; cond; step)` — detect via explicit `;`.
    if (p_match(p, TK_SEMI)) {
      init = first;
      cond = parse_expr(p, PREC_NONE);
      p_consume(p, TK_SEMI, "Expected ';' in for-style loop");
      step = parse_expr(p, PREC_NONE);
    } else {
      cond = first;
    }
    p_consume(p, TK_RPAREN, "Expected ')' after loop header");
  }

  AstNodeId body = parse_expr(p, PREC_NONE);

  uint32_t payload[4] = {init.idx, cond.idx, step.idx, body.idx};
  AstExtraDataIdx extra = ast_push_extra(p->ast, payload, 4);
  AstNodeData data = {0};
  data.extra_idx = extra;

  return p_push_node(p, AST_STMT_LOOP, op_index, data,
                     span_from_to(p, start_tok, p_prev(p)));
}

// =============================================================================
// struct { [pub?] name : T; ... }
//
// Extras layout (AST_DECL_STRUCT): [field_count, field0_id, field1_id, ...]
// Each field is AST_DECL_VAL with extras [name_id, type_id, vis_flag].
//
// Union sub-members and visibility on fields are recognized minimally.
// =============================================================================

// static AstNodeId parse_struct_expr(Parser *p) {
//     const Token *start_tok = p_advance(p); // consume TK_STRUCT
//     uint32_t op_index = p->pos - 1;

//     p_consume(p, TK_LBRACE, "Expected '{' after struct");

//     uint32_t fields[256];
//     uint32_t field_count = 0;

//     while (!p_is_eof(p) && p_peek(p) != TK_RBRACE) {
//         size_t pos_before = p->pos;

//         //bool is_pub = p_match(p, TK_PUB);
//         const Token *name_tok = p_consume(p, TK_IDENTIFIER, "Expected field
//         name"); if (!name_tok) break; AstNodeData nd = {0}; nd.string_id =
//         name_tok->string_id; AstNodeId name_id = p_push_node(p,
//         AST_EXPR_PATH, p->pos - 1, nd, p_span(p, name_tok, name_tok));

//         p_consume(p, TK_COLON, "Expected ':' after field name");
//         AstNodeId type_id = parse_type_expr(p);

//         uint32_t payload[3] = { name_id.idx, type_id.idx, is_pub ? 1u : 0u };
//         AstExtraDataIdx fextra = ast_push_extra(p->ast, payload, 3);
//         AstNodeData fdata = {0};
//         fdata.extra_idx = fextra;
//         AstNodeId field = p_push_node(p, AST_DECL_VAL, p->pos - 1, fdata,
//                                        span_from_to(p, name_tok, p_prev(p)));
//         if (field_count < 256) fields[field_count++] = field.idx;

//         p_match(p, TK_SEMI);
//         if (p->pos == pos_before) p_advance(p);  // forward progress
//     }

//     p_consume(p, TK_RBRACE, "Expected '}' to close struct");

//     uint32_t payload[257];
//     payload[0] = field_count;
//     for (uint32_t i = 0; i < field_count; i++) payload[1 + i] = fields[i];
//     AstExtraDataIdx extra = ast_push_extra(p->ast, payload, 1 + field_count);
//     AstNodeData data = {0};
//     data.extra_idx = extra;

//     return p_push_node(p, AST_DECL_STRUCT, op_index, data, span_from_to(p,
//     start_tok, p_prev(p)));
// }

// =============================================================================
// enum { Name [= value]; ... }
//
// Extras layout (AST_DECL_ENUM): [variant_count, variant0_id, ...]
// Each variant: AST_DECL_VAL with extras [name_id, explicit_value_id (or
// NONE)].
// =============================================================================

static AstNodeId parse_enum_expr(Parser *p) {
  const Token *start_tok = p_advance(p); // consume TK_ENUM
  uint32_t op_index = p->pos - 1;

  p_consume(p, TK_LBRACE, "Expected '{' after enum");

  // extras = [variant_count, variant0, ...] via scratch stack.
  uint32_t st = scratch_open(p);
  uint32_t cnt_at = scratch_reserve(p);
  uint32_t variant_count = 0;

  while (!p_is_eof(p) && p_peek(p) != TK_RBRACE) {
    size_t pos_before = p->pos;

    const Token *name_tok =
        p_consume(p, TK_IDENTIFIER, "Expected variant name");
    if (!name_tok)
      break;
    AstNodeData nd = {0};
    nd.string_id = name_tok->string_id;
    AstNodeId name_id = p_push_node(p, AST_EXPR_PATH, p->pos - 1, nd,
                                    p_span(p, name_tok, name_tok));

    AstNodeId value = AST_NODE_ID_NONE;
    if (p_match(p, TK_EQ)) {
      value = parse_expr(p, PREC_BITWISE);
    }

    uint32_t payload[2] = {name_id.idx, value.idx};
    AstExtraDataIdx vextra = ast_push_extra(p->ast, payload, 2);
    AstNodeData vdata = {0};
    vdata.extra_idx = vextra;
    AstNodeId variant = p_push_node(p, AST_DECL_VAL, p->pos - 1, vdata,
                                    span_from_to(p, name_tok, p_prev(p)));
    scratch_push(p, variant.idx);
    variant_count++;

    p_match(p, TK_SEMI);
    if (p->pos == pos_before)
      p_advance(p);
  }

  p_consume(p, TK_RBRACE, "Expected '}' to close enum");

  scratch_set(p, cnt_at, variant_count);
  AstExtraDataIdx extra = scratch_emit(p, st);
  AstNodeData data = {0};
  data.extra_idx = extra;

  return p_push_node(p, AST_DECL_ENUM, op_index, data,
                     span_from_to(p, start_tok, p_prev(p)));
}

// =============================================================================
// switch (scrutinee) { pat [| pat ...] => body; ... }
//
// Extras layout (AST_STMT_SWITCH): [scrutinee_id, arm_count, arm0_id, ...]
// Each arm: AST_DECL_VAL with extras [pat_count, pat0, pat1, ..., body_id]
// =============================================================================

static AstNodeId parse_switch_expr(Parser *p) {
  const Token *start_tok = p_advance(p); // consume TK_SWITCH
  uint32_t op_index = p->pos - 1;

  p_consume(p, TK_LPAREN, "Expected '(' after switch");
  AstNodeId scrutinee = parse_expr(p, PREC_NONE);
  p_consume(p, TK_RPAREN, "Expected ')' after switch scrutinee");

  p_consume(p, TK_LBRACE, "Expected '{' to open switch body");

  // extras = [scrutinee, arm_count, arm0, ...]. scrutinee is known
  // now; arm_count backpatched after the loop.
  uint32_t st = scratch_open(p);
  scratch_push(p, scrutinee.idx);
  uint32_t ac_at = scratch_reserve(p);
  uint32_t arm_count = 0;

  while (!p_is_eof(p) && p_peek(p) != TK_RBRACE) {
    size_t pos_before = p->pos;
    const Token *arm_start = p_current(p);

    // Per-arm nested region: [pat_count, pat0..patN, body].
    uint32_t ast2 = scratch_open(p);
    uint32_t pc_at = scratch_reserve(p);
    uint32_t pat_count = 0;
    for (;;) {
      AstNodeId pat = parse_expr(p, PREC_OR + 1); // stop before `|`
      if (pat.idx) {
        scratch_push(p, pat.idx);
        pat_count++;
      }
      if (!p_match(p, TK_PIPE))
        break;
    }

    p_consume(p, TK_FATARROW, "Expected '=>' in switch arm");
    AstNodeId body = parse_expr(p, PREC_NONE);
    scratch_push(p, body.idx);
    scratch_set(p, pc_at, pat_count);
    AstExtraDataIdx aextra = scratch_emit(p, ast2);

    AstNodeData adata = {0};
    adata.extra_idx = aextra;
    AstNodeId arm = p_push_node(p, AST_DECL_VAL, p->pos - 1, adata,
                                span_from_to(p, arm_start, p_prev(p)));
    scratch_push(p, arm.idx);
    arm_count++;

    p_match(p, TK_SEMI);
    if (p->pos == pos_before)
      p_advance(p);
  }

  p_consume(p, TK_RBRACE, "Expected '}' to close switch");

  scratch_set(p, ac_at, arm_count);
  AstExtraDataIdx extra = scratch_emit(p, st);
  AstNodeData data = {0};
  data.extra_idx = extra;

  return p_push_node(p, AST_STMT_SWITCH, op_index, data,
                     span_from_to(p, start_tok, p_prev(p)));
}

// =============================================================================
// [N]T   — array type / array literal (when followed by `{...}`)
// []T    — slice type
// [^]T   — many-pointer type
// [_]T{} — array literal with inferred size (value-position only)
//
// Array-literal recognition is deferred to step 5 (postfix-LBRACE in the
// Pratt loop). For now we only build the type expressions.
// =============================================================================

static AstNodeId parse_bracket_expr(Parser *p) {
  const Token *start_tok = p_advance(p); // consume TK_LBRACKET
  uint32_t op_index = p->pos - 1;

  // [^]T — many-pointer
  if (p_match(p, TK_CARET)) {
    p_consume(p, TK_RBRACKET, "Expected ']' after '[^'");
    AstNodeId elem = parse_type_expr(p);
    AstNodeData data = {0};
    data.single_child = elem;
    return p_push_node(p, AST_TYPE_MANYPTR, op_index, data,
                       span_from_to(p, start_tok, p_prev(p)));
  }

  // []T — slice
  if (p_match(p, TK_RBRACKET)) {
    AstNodeId elem = parse_type_expr(p);
    AstNodeData data = {0};
    data.single_child = elem;
    return p_push_node(p, AST_TYPE_SLICE, op_index, data,
                       span_from_to(p, start_tok, p_prev(p)));
  }

  // [_]T — inferred-size array (value-only)
  if (p_match(p, TK_UNDERSCORE)) {
    p_consume(p, TK_RBRACKET, "Expected ']' after '[_'");
    AstNodeId elem = parse_type_expr(p);
    // Step 5 will pick up the trailing `{...}` literal initializer; we
    // emit a placeholder array-type node for now.
    uint32_t payload[2] = {AST_NODE_ID_NONE.idx, elem.idx};
    AstExtraDataIdx extra = ast_push_extra(p->ast, payload, 2);
    AstNodeData data = {0};
    data.extra_idx = extra;
    return p_push_node(p, AST_TYPE_ARRAY, op_index, data,
                       span_from_to(p, start_tok, p_prev(p)));
  }

  // [N]T — sized array
  AstNodeId size = parse_expr(p, PREC_BITWISE);
  p_consume(p, TK_RBRACKET, "Expected ']' after array size");
  AstNodeId elem = parse_type_expr(p);
  uint32_t payload[2] = {size.idx, elem.idx};
  AstExtraDataIdx extra = ast_push_extra(p->ast, payload, 2);
  AstNodeData data = {0};
  data.extra_idx = extra;
  return p_push_node(p, AST_TYPE_ARRAY, op_index, data,
                     span_from_to(p, start_tok, p_prev(p)));
}

// =============================================================================
// .{ ... }     — anonymous product literal
// .Variant     — enum reference
// =============================================================================

static AstNodeId parse_dot_expr(Parser *p) {
  const Token *start_tok = p_advance(p); // consume TK_DOT
  uint32_t op_index = p->pos - 1;

  // .{ ... } anonymous product
  if (p_match(p, TK_LBRACE)) {
    // extras = [type_expr, field_count, field0, ...]; type slot is
    // NONE for anonymous `.{ }`. Slots backpatched after the loop.
    uint32_t st = scratch_open(p);
    scratch_push(p, AST_NODE_ID_NONE.idx); // type_expr (none)
    uint32_t cnt_at = scratch_reserve(p);
    uint32_t field_count = 0;
    while (!p_is_eof(p) && p_peek(p) != TK_RBRACE) {
      size_t pos_before = p->pos;
      // Optional `.name =` prefix for named fields. Otherwise positional.
      AstNodeId field_name = AST_NODE_ID_NONE;
      if (p_peek(p) == TK_DOT && p_peek_at(p, 1) == TK_IDENTIFIER) {
        p_advance(p); // .
        const Token *nm = p_advance(p);
        AstNodeData nd = {0};
        nd.string_id = nm->string_id;
        field_name =
            p_push_node(p, AST_EXPR_PATH, p->pos - 1, nd, p_span(p, nm, nm));
        p_consume(p, TK_EQ, "Expected '=' after field name");
      }
      AstNodeId value = parse_expr(p, PREC_NONE);
      uint32_t payload[2] = {field_name.idx, value.idx};
      AstExtraDataIdx fextra = ast_push_extra(p->ast, payload, 2);
      AstNodeData fdata = {0};
      fdata.extra_idx = fextra;
      AstNodeId field = p_push_node(p, AST_DECL_VAL, p->pos - 1, fdata,
                                    span_from_to(p, p_current(p), p_prev(p)));
      scratch_push(p, field.idx);
      field_count++;
      if (!p_match(p, TK_COMMA))
        break;
      if (p->pos == pos_before)
        p_advance(p);
    }
    // Layout injects `;` before `}` in brace-delimited lists.
    while (p_match(p, TK_SEMI)) { /* skip */
    }
    p_consume(p, TK_RBRACE, "Expected '}' to close product literal");

    scratch_set(p, cnt_at, field_count);
    AstExtraDataIdx extra = scratch_emit(p, st);
    AstNodeData data = {0};
    data.extra_idx = extra;
    return p_push_node(p, AST_EXPR_PRODUCT, op_index, data,
                       span_from_to(p, start_tok, p_prev(p)));
  }

  // .Variant — enum ref
  if (p_peek(p) == TK_IDENTIFIER) {
    const Token *nm = p_advance(p);
    AstNodeData d = {0};
    d.string_id = nm->string_id;
    return p_push_node(p, AST_EXPR_ENUM_REF, op_index, d,
                       span_from_to(p, start_tok, nm));
  }

  p_error(p, "Unexpected '.'");
  return AST_NODE_ID_NONE;
}

// =============================================================================
// @name(args?) — compiler builtin
//
// Extras layout (AST_EXPR_BUILTIN): [name_string_id, arg_count, arg0, ...]
// The name lives in the extras (StrId.idx fits in u32).
// =============================================================================

static AstNodeId parse_builtin_expr(Parser *p) {
  const Token *start_tok = p_advance(p); // consume TK_AT
  uint32_t op_index = p->pos - 1;

  const Token *name_tok =
      p_consume(p, TK_IDENTIFIER, "Expected builtin name after '@'");
  if (!name_tok)
    return AST_NODE_ID_NONE;

  // extras = [name_strid, arg_count, arg0, ...] via scratch stack.
  uint32_t st = scratch_open(p);
  scratch_push(p, name_tok->string_id.idx);
  uint32_t cnt_at = scratch_reserve(p);
  uint32_t arg_count = 0;
  if (p_match(p, TK_LPAREN)) {
    while (!p_is_eof(p) && p_peek(p) != TK_RPAREN) {
      AstNodeId arg = parse_expr(p, PREC_NONE);
      if (arg.idx) {
        scratch_push(p, arg.idx);
        arg_count++;
      }
      if (!p_match(p, TK_COMMA))
        break;
    }
    p_consume(p, TK_RPAREN, "Expected ')' after builtin args");
  }

  scratch_set(p, cnt_at, arg_count);
  AstExtraDataIdx extra = scratch_emit(p, st);
  AstNodeData data = {0};
  data.extra_idx = extra;

  return p_push_node(p, AST_EXPR_BUILTIN, op_index, data,
                     span_from_to(p, start_tok, p_prev(p)));
}

// =============================================================================
// Prefix unary helper. Used for both value-position (- ! ~ & *) and
// type-position (^T ?T const T) forms. The TokenKind picks the AST kind.
// =============================================================================

static AstNodeId parse_prefix_unary(Parser *p, AstNodeKind kind) {
  const Token *start_tok = p_advance(p);
  uint32_t op_index = p->pos - 1;

  AstNodeId operand = parse_expr(p, PREC_UNARY);
  if (operand.idx == 0)
    return AST_NODE_ID_NONE;

  AstNodeData data = {0};
  data.single_child = operand;
  TinySpan op_span = p_node_span(p, operand);
  TinySpan full = span_make_range((uint16_t)p->file.idx, start_tok->start,
                                  span_end(op_span));
  return p_push_node(p, kind, op_index, data, full);
}

// =============================================================================
// Statement-shaped expressions: return / defer / break / continue
// =============================================================================

static AstNodeId parse_return_expr(Parser *p) {
  const Token *start_tok = p_advance(p);
  uint32_t op_index = p->pos - 1;
  AstNodeId value = AST_NODE_ID_NONE;
  TokenKind nx = p_peek(p);
  if (nx != TK_SEMI && nx != TK_RBRACE && nx != TK_EOF) {
    value = parse_expr(p, PREC_NONE);
  }
  AstNodeData data = {0};
  data.single_child = value;
  return p_push_node(p, AST_STMT_RETURN, op_index, data,
                     span_from_to(p, start_tok, p_prev(p)));
}

static AstNodeId parse_defer_expr(Parser *p) {
  const Token *start_tok = p_advance(p);
  uint32_t op_index = p->pos - 1;
  AstNodeId inner = parse_expr(p, PREC_NONE);
  AstNodeData data = {0};
  data.single_child = inner;
  return p_push_node(p, AST_STMT_DEFER, op_index, data,
                     span_from_to(p, start_tok, p_prev(p)));
}

static AstNodeId parse_break_or_continue(Parser *p, AstNodeKind kind) {
  const Token *start_tok = p_advance(p);
  uint32_t op_index = p->pos - 1;
  AstNodeData data = {0};
  return p_push_node(p, kind, op_index, data, p_span(p, start_tok, start_tok));
}

// =============================================================================
// Block expression: `{ stmts; ... }` as a value. Reuses parse_block from
// parse_stmt.c which already consumes `{` ... `}` and packs the statements
// into AST_STMT_BLOCK.
// =============================================================================

// (defined in parse_stmt.c)
extern AstNodeId parse_block(Parser *p);

// =============================================================================
// Thin type wrapper: parse_type_expr is just `parse_expr` with the
// parsing_type flag set, restored on exit. Mirrors GRAMMAR.md §4.1.
// =============================================================================

AstNodeId parse_type_expr(Parser *p) {
  bool saved = p->parsing_type;
  p->parsing_type = true;
  AstNodeId t = parse_expr(p, PREC_BITWISE);
  p->parsing_type = saved;
  return t;
}

// True when the cursor is on the layout `;` that terminates the
// ENCLOSING block's statement (`; }` or `; <eof>`). `with`'s tail
// consumption must leave this `;` for the caller (parse_block /
// top-level loop mandate exactly one terminator per statement, by
// design). Bounded 2-token peek, non-consuming, no backtracking.
static inline bool at_block_terminator(const Parser *p) {
  return p_peek(p) == TK_SEMI &&
         (p_peek_at(p, 1) == TK_RBRACE || p_peek_at(p, 1) == TK_EOF);
}

// =============================================================================
// `with` — continuation capture (Koka `with`/applyToContinuation;
// old parser parser.c.backup:1721). `with [p :=] EXPR` consumes the
// REST of the enclosing block as the continuation and desugars to
// `EXPR(args.., fn([p]) { rest })` via the shared emit_trailing_call.
// NOT its own AST node — pure parse-time desugar, same shape as the
// `<-` trailing lambda except the body is the implicit block-tail.
// A nested `with` in the tail recurses naturally. Binder uses `:=`
// (its existing "bind, type inferred" meaning) — NOT `<-`, which is
// solely the trailing-lambda operator (no re-overload).
// =============================================================================
static AstNodeId parse_with_stmt(Parser *p) {
  const Token *with_tok = p_advance(p); // consume `with`
  uint32_t op_index = p->pos - 1;

  // Optional binder `with p := EXPR`. One-shot 2-token peek (`IDENT`
  // then `:=`) — non-consuming, no backtracking. `:=` keeps its
  // "local binding, type inferred" meaning; the binder names the
  // continuation lambda's parameter. parse_param reads `p` (name only,
  // no type — `:=` is inference) and stops at `:=`.
  AstNodeId binder = AST_NODE_ID_NONE;
  uint32_t param_count = 0;
  if (p_peek(p) == TK_IDENTIFIER && p_peek_at(p, 1) == TK_COLON_EQ) {
    binder = parse_param(p, /*name_required=*/true);
    p_consume(p, TK_COLON_EQ, "expected ':=' after with-binder");
    if (binder.idx)
      param_count = 1;
  }

  // The with-expression (the call receiving the continuation). Ends at
  // the statement boundary; the layout `;` is skipped next.
  AstNodeId head = parse_expr(p, PREC_NONE);
  if (head.idx == 0) {
    p_error(p, "expected an expression after 'with'");
    return AST_NODE_ID_NONE;
  }
  // Consume the head→tail separator `;` — UNLESS the tail is empty, in
  // which case that `;` is the with-statement's own terminator (leave
  // it for the enclosing block).
  if (!at_block_terminator(p))
    p_match(p, TK_SEMI);

  // Consume the REST of the enclosing block as the continuation body.
  // Leave the FINAL block-terminating `;` for the caller's parse_block
  // (its mandatory terminator is by-design — see parse_stmt.c). A
  // nested `with` recurses through parse_expr here.
  uint32_t bst = scratch_open(p);
  uint32_t bcnt_at = scratch_reserve(p);
  uint32_t stmt_count = 0;
  while (!p_is_eof(p) && p_peek(p) != TK_RBRACE && !at_block_terminator(p)) {
    uint32_t before = p->pos;
    AstNodeId stmt = parse_expr(p, PREC_NONE);
    if (stmt.idx != 0) {
      scratch_push(p, stmt.idx);
      stmt_count++;
    }
    if (at_block_terminator(p))
      break; // leave the with-statement's terminator `;`
    p_match(p, TK_SEMI);
    if (p->pos == before)
      p_advance(p);
  }
  scratch_set(p, bcnt_at, stmt_count);
  AstExtraDataIdx bex = scratch_emit(p, bst);
  AstNodeData bdata = {0};
  bdata.extra_idx = bex;
  const Token *end_tok = p_prev(p);
  TinySpan span =
      span_make_range((uint16_t)p->file.idx, with_tok->start,
                      end_tok ? end_tok->byte_end : with_tok->byte_end);
  AstNodeId body = p_push_node(p, AST_STMT_BLOCK, op_index, bdata, span);

  // Continuation lambda: extras [ret, body, eff, pc, binder?].
  uint32_t lst = scratch_open(p);
  uint32_t h_ret = scratch_reserve(p);
  uint32_t h_body = scratch_reserve(p);
  uint32_t h_eff = scratch_reserve(p);
  uint32_t h_pc = scratch_reserve(p);
  if (param_count)
    scratch_push(p, binder.idx);
  scratch_set(p, h_ret, AST_NODE_ID_NONE.idx);
  scratch_set(p, h_body, body.idx);
  scratch_set(p, h_eff, AST_NODE_ID_NONE.idx);
  scratch_set(p, h_pc, param_count);
  AstExtraDataIdx lex = scratch_emit(p, lst);
  AstNodeData ldata = {0};
  ldata.extra_idx = lex;
  AstNodeId lambda = p_push_node(p, AST_EXPR_LAMBDA, op_index, ldata, span);

  return emit_trailing_call(p, head, lambda, op_index, span);
}

// =============================================================================
// parse_prefix — the single-token dispatcher for the prefix position.
// =============================================================================

static AstNodeId parse_prefix(Parser *p) {
  const Token *start_tok = p_current(p);
  TokenKind kind = start_tok->kind;
  uint32_t op_index = p->pos;

  switch (kind) {

  // ---- Literals --------------------------------------------------
  case TK_INT_LIT:
  case TK_FLOAT_LIT: {
    p_advance(p);
    AstNodeData d = {0};
    d.string_id = start_tok->string_id;
    AstNodeKind k =
        (kind == TK_INT_LIT) ? AST_EXPR_LIT_INT : AST_EXPR_LIT_FLOAT;
    return p_push_node(p, k, op_index, d, p_span(p, start_tok, start_tok));
  }
  case TK_STRING_LIT:
  case TK_BYTE_LIT: {
    p_advance(p);
    AstNodeData d = {0};
    d.string_id = start_tok->string_id;
    AstNodeKind k =
        (kind == TK_STRING_LIT) ? AST_EXPR_LIT_STRING : AST_EXPR_LIT_BYTE;
    return p_push_node(p, k, op_index, d, p_span(p, start_tok, start_tok));
  }
  case TK_ASM_LIT: {
    p_advance(p);
    AstNodeData d = {0};
    d.string_id = start_tok->string_id;
    return p_push_node(p, AST_EXPR_ASM, op_index, d,
                       p_span(p, start_tok, start_tok));
  }
  case TK_TRUE:
  case TK_FALSE: {
    p_advance(p);
    AstNodeData d = {0};
    d.bool_val = (kind == TK_TRUE);
    return p_push_node(p, AST_EXPR_LIT_BOOL, op_index, d,
                       p_span(p, start_tok, start_tok));
  }
  case TK_NIL: {
    p_advance(p);
    AstNodeData d = {0};
    return p_push_node(p, AST_EXPR_LIT_NIL, op_index, d,
                       p_span(p, start_tok, start_tok));
  }

  // ---- Type-position keyword idents ------------------------------
  // `void`, `noreturn`, `anytype`, `type` are values in type position
  // (the body of `name : T`). We emit them as TYPE_PATH for now;
  // disambiguation against value-context use is the resolver's job.
  case TK_VOID:
  case TK_NORETURN:
  case TK_ANYTYPE:
  case TK_TYPE: {
    p_advance(p);
    return emit_ident(p, start_tok, AST_TYPE_PATH);
  }

  case TK_UNDERSCORE: {
    p_advance(p);
    AstNodeData d = {0};
    return p_push_node(p, AST_EXPR_WILDCARD, op_index, d,
                       p_span(p, start_tok, start_tok));
  }

  case TK_IDENTIFIER: {
    p_advance(p);
    return emit_ident(p, start_tok, AST_EXPR_PATH);
  }

  // ---- Grouping --------------------------------------------------
  case TK_LPAREN: {
    p_advance(p);
    AstNodeId inner = parse_expr(p, PREC_NONE);
    const Token *end_tok =
        p_consume(p, TK_RPAREN, "Expected ')' after expression");
    if (!end_tok)
      return AST_NODE_ID_NONE;
    AstNodeData d = {0};
    d.single_child = inner;
    return p_push_node(p, AST_EXPR_GROUP, op_index, d,
                       p_span(p, start_tok, end_tok));
  }

  // ---- Block-as-expression ---------------------------------------
  case TK_LBRACE:
    return parse_block(p);

  // ---- Prefix unary (value position) -----------------------------
  case TK_MINUS:
    return parse_prefix_unary(p, AST_EXPR_UNARY_NEG);
  case TK_BANG:
    return parse_prefix_unary(p, AST_EXPR_UNARY_NOT);
  case TK_TILDE:
    return parse_prefix_unary(p, AST_EXPR_UNARY_BIT_NOT);
  case TK_AMP:
    return parse_prefix_unary(p, AST_EXPR_UNARY_REF);
  case TK_STAR:
    return parse_prefix_unary(p, AST_EXPR_UNARY_DEREF);

  // ---- Prefix unary (type position) ------------------------------
  case TK_CARET:
    return parse_prefix_unary(p, AST_EXPR_UNARY_PTR);
  case TK_QUESTION:
    return parse_prefix_unary(p, AST_EXPR_UNARY_OPTIONAL);
  case TK_CONST:
    return parse_prefix_unary(p, AST_EXPR_UNARY_CONST);

  // ---- Decl-shaped expressions -----------------------------------
  case TK_FN:
    return parse_fn_lambda(p);
  case TK_FN_TYPE:
    return parse_fn_type(p);
  // case TK_STRUCT:    return parse_struct_expr(p);
  case TK_ENUM:
    return parse_enum_expr(p);
  case TK_SWITCH:
    return parse_switch_expr(p);

  // ---- Control flow ----------------------------------------------
  case TK_IF:
  case TK_ELIF:
    return parse_if_expr(p);
  case TK_LOOP:
    return parse_loop_expr(p);
  case TK_RETURN:
    return parse_return_expr(p);
  case TK_DEFER:
    return parse_defer_expr(p);
  case TK_BREAK:
    return parse_break_or_continue(p, AST_STMT_BREAK);
  case TK_CONTINUE:
    return parse_break_or_continue(p, AST_STMT_CONTINUE);

  // ---- Array / bracket forms -------------------------------------
  case TK_LBRACKET:
    return parse_bracket_expr(p);

  // ---- Dot forms -------------------------------------------------
  case TK_DOT:
    return parse_dot_expr(p);

  // ---- Compiler builtins -----------------------------------------
  case TK_AT:
    return parse_builtin_expr(p);

  // ---- comptime expr ---------------------------------------------
  // Currently a passthrough — the inner expr's comptime-ness gets
  // tracked when ModuleInfo grows a parallel is_comptime vec. For now
  // we drop the marker silently.
  case TK_COMPTIME: {
    p_advance(p);
    return parse_prefix(p);
  }

  // ---- `with` — continuation capture (implemented) ---------------
  case TK_WITH:
    return parse_with_stmt(p);

  // ---- Effects / handlers / mask — deferred ----------------------
  case TK_EFFECT:
  case TK_HANDLER:
  case TK_HANDLE:
  case TK_MASK: {
    p_error(p,
            "effect / handler / mask not yet implemented in new parser");
    p_advance(p);
    return AST_NODE_ID_NONE;
  }

  default:
    p_error(p, "Expected expression");
    p_advance(p); // forward progress on unrecognized token
    return AST_NODE_ID_NONE;
  }
}

// Append `lambda` as the trailing argument of `left`: if `left` is
// already AST_EXPR_CALL, rebuild its extras [callee, argc+1, args..,
// lambda]; otherwise wrap `left(lambda)`. The old call node is left
// orphaned in the store (inert, append-only model). `ed` is refetched
// after scratch_emit-free reads to stay valid across reallocs. Shared
// by the `<-` parselet and `with`.
static AstNodeId emit_trailing_call(Parser *p, AstNodeId left,
                                    AstNodeId lambda, uint32_t op_index,
                                    TinySpan span) {
  AstNodeKind lk = ((AstNodeKind *)p->ast->kinds.data)[left.idx];
  uint32_t cst = scratch_open(p);
  uint32_t c_cnt_at;
  uint32_t new_argc;
  if (lk == AST_EXPR_CALL) {
    uint32_t base = ((AstNodeData *)p->ast->data.data)[left.idx].extra_idx.idx;
    const uint32_t *ed = (const uint32_t *)p->ast->extra.data;
    uint32_t callee_idx = ed[base + 0];
    uint32_t old_argc = ed[base + 1];
    scratch_push(p, callee_idx);
    c_cnt_at = scratch_reserve(p);
    for (uint32_t i = 0; i < old_argc; i++)
      scratch_push(p, ed[base + 2 + i]);
    scratch_push(p, lambda.idx);
    new_argc = old_argc + 1;
  } else {
    scratch_push(p, left.idx);
    c_cnt_at = scratch_reserve(p);
    scratch_push(p, lambda.idx);
    new_argc = 1;
  }
  scratch_set(p, c_cnt_at, new_argc);
  AstExtraDataIdx cex = scratch_emit(p, cst);
  AstNodeData cdata = {0};
  cdata.extra_idx = cex;
  return p_push_node(p, AST_EXPR_CALL, op_index, cdata, span);
}

// =============================================================================
// parse_infix — postfix call + binary infix.
//
// Step 5 will replace this with proper postfix handlers for `.`, `->`,
// `[`, `{` (trailing-lambda + struct-literal lookahead), `++`, `^`, `?`,
// `!`, plus the bind operators `::`, `:=`, `:` at PREC_NONE.
// =============================================================================

static AstNodeId parse_infix(Parser *p, AstNodeId left, TinySpan left_span) {
  TokenKind kind = p_peek(p);
  uint32_t op_index = p->pos;
  p_advance(p);

  // Function call.
  if (kind == TK_LPAREN) {
    // extras = [callee, arg_count, arg0, ...] via scratch stack.
    uint32_t st = scratch_open(p);
    scratch_push(p, left.idx);
    uint32_t cnt_at = scratch_reserve(p);
    uint32_t arg_count = 0;

    while (!p_is_eof(p) && p_peek(p) != TK_RPAREN) {
      AstNodeId arg = parse_expr(p, PREC_NONE);
      scratch_push(p, arg.idx);
      arg_count++;
      if (!p_match(p, TK_COMMA))
        break;
    }

    const Token *end_tok =
        p_consume(p, TK_RPAREN, "Expected ')' after arguments");
    if (!end_tok) {
      p->scratch.count = st;
      return left;
    }

    scratch_set(p, cnt_at, arg_count);
    AstExtraDataIdx extra = scratch_emit(p, st);
    AstNodeData data = {0};
    data.extra_idx = extra;

    TinySpan full_span = span_make_range(
        (uint16_t)p->file.idx, span_start(left_span), end_tok->byte_end);
    return p_push_node(p, AST_EXPR_CALL, op_index, data, full_span);
  }

  // Bind: `name :: v` / `name := v` / `name : T [: | =] v` / `name : T`.
  // Guarded low-precedence infix: only valid when `left` is a bare name
  // (AST_EXPR_PATH). PREC_BIND being lowest means a wider LHS like
  // `3 + 2` has already collapsed into `left`, so the kind check below
  // yields a precise "name expected" error instead of a garbled parse.
  // Destructure-product patterns (`.{a,b} :=`) are a TODO.
  if (kind == TK_COLON_COLON || kind == TK_COLON_EQ || kind == TK_COLON) {
    AstNodeKind lk = ((AstNodeKind *)p->ast->kinds.data)[left.idx];
    if (lk != AST_EXPR_PATH) {
      p_error(p, "expected a name before bind operator");
      return AST_NODE_ID_NONE;
    }
    StrId name_sid = ((AstNodeData *)p->ast->data.data)[left.idx].string_id;
    uint32_t name_tok_idx = ((uint32_t *)p->ast->main_tokens.data)[left.idx];

    bool is_const = false;
    AstNodeId type_id = AST_NODE_ID_NONE;
    bool has_value = true;

    if (kind == TK_COLON_COLON) {
      is_const = true;
    } else if (kind == TK_COLON_EQ) {
      is_const = false;
    } else { // TK_COLON — typed
      type_id = parse_type_expr(p);
      if (p_match(p, TK_COLON))
        is_const = true; // name : T : v
      else if (p_match(p, TK_EQ))
        is_const = false; // name : T = v
      else {
        is_const = false;
        has_value = false;
      } // name : T
    }

    // Modifiers: comptime/distinct are hard keywords; the rest are
    // contextual (lex as TK_IDENTIFIER, matched via p->s->names).
    DefMeta meta = 0;
    bool gathering = true;
    while (gathering) {
      const Token *t = p_current(p);
      switch (t->kind) {
      case TK_COMPTIME:
        meta |= META_COMPTIME;
        p_advance(p);
        break;
      case TK_IDENTIFIER: {
        StrId s = t->string_id;
        const DbNames *N = &p->s->names;
        if (s.idx == N->PUB.idx) {
          meta &= ~META_VIS_MASK;
          meta |= VIS_PUBLIC;
        } else if (s.idx == N->PVT.idx) {
          meta &= ~META_VIS_MASK;
        } else if (s.idx == N->ABSTRACT.idx) {
          meta &= ~META_VIS_MASK;
          meta |= VIS_INTERNAL;
        } else if (s.idx == N->NAMED.idx) {
          meta |= META_NAMED;
        } else if (s.idx == N->SCOPED.idx) {
          meta |= META_SCOPED;
        } else if (s.idx == N->LINEAR.idx) {
          meta |= META_LINEAR;
        } else if (s.idx == N->DISTINCT.idx) {
          meta |= META_DISTINCT;
        } else {
          gathering = false;
          break;
        }
        p_advance(p);
        break;
      }
      default:
        gathering = false;
        break;
      }
    }

    AstNodeId value = AST_NODE_ID_NONE;
    if (has_value) {
      // RHS parses at PREC_BIND so it grabs a full expression but
      // does NOT swallow a following sibling bind.
      value = parse_expr(p, PREC_BIND);
      if (value.idx == 0) {
        p_error(p, "expected expression after bind operator");
        return AST_NODE_ID_NONE;
      }
    }

    uint32_t payload[4] = {name_sid.idx, type_id.idx, value.idx,
                           (uint32_t)meta};
    AstExtraDataIdx ex = ast_push_extra(p->ast, payload, 4);
    AstNodeData data = {0};
    data.extra_idx = ex;

    AstNodeKind dk = is_const ? AST_DECL_CONST : AST_DECL_VAR;
    const Token *end_tok = vec_get((Vec *)p->tokens, p->pos - 1);
    TinySpan full = span_make_range((uint16_t)p->file.idx,
                                    span_start(left_span), end_tok->byte_end);
    return p_push_node(p, dk, name_tok_idx, data, full);
  }

  // Trailing lambda: `callee <- { body }` ⇒ `callee(fn() { body })`,
  // `callee(args) <- (p) { body }` ⇒ `callee(args, fn(p) { body })`.
  // `<-` already put us in lambda-tail mode, so an optional `(params)`
  // then a block body parse unambiguously (no cover grammar). Params
  // use `( )` not `| |`: `|` is a layout end-continuation (it's
  // bitwise-or), so a braceless `<- |x|\n  body` would fuse lines; `)`
  // is start-continuation only, so `<- (x)\n  body` opens the body
  // block correctly. Same delimiter + parse_param as `fn(params)`.
  // `<-` (not `=>`): reads "body into callee" (the lambda IS an arg of
  // callee); `=>` stays the switch arm separator only. Desugar is the
  // shared emit_trailing_call (also used by `with`).
  if (kind == TK_LARROW) {
    const Token *arrow_tok = vec_get((Vec *)p->tokens, op_index);

    // --- Build the lambda. Extras: [ret, body, eff, pc, params...]. ---
    uint32_t lst = scratch_open(p);
    uint32_t h_ret = scratch_reserve(p);
    uint32_t h_body = scratch_reserve(p);
    uint32_t h_eff = scratch_reserve(p);
    uint32_t h_pc = scratch_reserve(p);

    uint32_t param_count = 0;
    if (p_match(p, TK_LPAREN)) {
      while (!p_is_eof(p) && p_peek(p) != TK_RPAREN) {
        AstNodeId prm = parse_param(p, /*name_required=*/true);
        if (prm.idx) {
          scratch_push(p, prm.idx);
          param_count++;
        }
        if (!p_match(p, TK_COMMA))
          break;
      }
      p_consume(p, TK_RPAREN, "Expected ')' to close lambda params");
    }

    // Body is ALWAYS a block — explicit `{ }` or layout-synthesized.
    // No bare-expression body (deliberate; see GRAMMAR notes).
    AstNodeId body = AST_NODE_ID_NONE;
    if (p_peek(p) == TK_LBRACE) {
      body = parse_block(p);
    } else {
      p_error(p, "expected '{ ... }' (or an indented block) after '<-'");
    }

    scratch_set(p, h_ret, AST_NODE_ID_NONE.idx);
    scratch_set(p, h_body, body.idx);
    scratch_set(p, h_eff, AST_NODE_ID_NONE.idx);
    scratch_set(p, h_pc, param_count);
    AstExtraDataIdx lex = scratch_emit(p, lst);
    AstNodeData ldata = {0};
    ldata.extra_idx = lex;
    const Token *end_tok = p_prev(p);
    TinySpan lspan = span_make_range((uint16_t)p->file.idx, arrow_tok->start,
                                     end_tok->byte_end);
    AstNodeId lambda = p_push_node(p, AST_EXPR_LAMBDA, op_index, ldata, lspan);

    TinySpan cspan = span_make_range(
        (uint16_t)p->file.idx, span_start(left_span), end_tok->byte_end);
    return emit_trailing_call(p, left, lambda, op_index, cspan);
  }

  // Binary / assignment.
  Precedence prec = get_infix_precedence(kind);
  int next_prec = is_right_associative(kind) ? (int)prec - 1 : (int)prec;

  AstNodeId right = parse_expr(p, next_prec);
  if (right.idx == 0)
    return left;

  AstNodeData data = {0};
  data.bin.lhs = left;
  data.bin.rhs = right;

  AstNodeKind ast_kind = get_binary_op_kind(kind);

  TinySpan right_span = p_node_span(p, right);
  TinySpan full_span = span_make_range(
      (uint16_t)p->file.idx, span_start(left_span), span_end(right_span));

  return p_push_node(p, ast_kind, op_index, data, full_span);
}

// =============================================================================
// parse_expr — Pratt loop.
// =============================================================================

AstNodeId parse_expr(Parser *p, int precedence) {
  AstNodeId left = parse_prefix(p);
  if (left.idx == 0)
    return left;

  TinySpan left_span = p_node_span(p, left);

  for (;;) {
    TokenKind tk = p_peek(p);

    // Postfix forms bind tighter than any binary op. Only call (LPAREN)
    // is wired today; step 5 adds DOT field, LBRACKET index/slice, and
    // the postfix unary family (`++` / `^` / `?` / `!`).
    if (precedence < PREC_POSTFIX && tk == TK_LPAREN) {
      left = parse_infix(p, left, left_span);
      left_span = p_node_span(p, left);
      continue;
    }

    // Binary / assignment infix.
    Precedence prec = get_infix_precedence(tk);
    if (prec == PREC_NONE || (int)prec <= precedence)
      break;

    left = parse_infix(p, left, left_span);
    left_span = p_node_span(p, left);
  }

  return left;
}
