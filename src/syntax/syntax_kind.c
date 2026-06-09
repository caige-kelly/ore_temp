#include "syntax_kind.h"

// =====================================================================
// OreSyntaxKind implementation: name table + classifier predicates.
// =====================================================================
//
// `ore_syntax_kind_name` is a single switch over every defined value.
// Every classifier predicate is implemented as either a range check
// or an explicit enumeration depending on whether the kinds in that
// category are contiguous in the enum.
//
// Adding a new kind: extend the enum in syntax_kind.h AND add the
// matching case here. The exhaustiveness test in tools/syntax_kind_test.c
// will fail at runtime if you forget (it asserts every value in range
// returns a non-"?" name).

const char *ore_syntax_kind_name(OreSyntaxKind k) {
  switch (k) {
  case SK_NONE:
    return "NONE";

  // Trivia + lex errors
  case SK_WHITESPACE:
    return "WHITESPACE";
  case SK_NEWLINE:
    return "NEWLINE";
  case SK_COMMENT:
    return "COMMENT";
  case SK_LEX_ERROR:
    return "LEX_ERROR";
  case SK_EOF:
    return "EOF";

  // Virtual layout
  case SK_VIRTUAL_LBRACE:
    return "VIRTUAL_LBRACE";
  case SK_VIRTUAL_RBRACE:
    return "VIRTUAL_RBRACE";
  case SK_VIRTUAL_SEMI:
    return "VIRTUAL_SEMI";

  // Identifier + literal lexemes
  case SK_IDENT:
    return "IDENT";
  case SK_INT_LIT:
    return "INT_LIT";
  case SK_FLOAT_LIT:
    return "FLOAT_LIT";
  case SK_STRING_LIT:
    return "STRING_LIT";
  case SK_BYTE_LIT:
    return "BYTE_LIT";
  case SK_ASM_LIT:
    return "ASM_LIT";

  // Keyword literals
  case SK_TRUE_KW:
    return "TRUE_KW";
  case SK_FALSE_KW:
    return "FALSE_KW";
  case SK_NIL_KW:
    return "NIL_KW";

  // Declaration keywords
  case SK_FN_KW:
    return "FN_KW";
  case SK_FN_TYPE_KW:
    return "FN_TYPE_KW";
  case SK_CONST_KW:
    return "CONST_KW";
  case SK_STRUCT_KW:
    return "STRUCT_KW";
  case SK_ENUM_KW:
    return "ENUM_KW";
  case SK_UNION_KW:
    return "UNION_KW";
  case SK_EFFECT_KW:
    return "EFFECT_KW";
  case SK_HANDLER_KW:
    return "HANDLER_KW";
  case SK_COMPTIME_KW:
    return "COMPTIME_KW";

  // Control-flow keywords
  case SK_IF_KW:
    return "IF_KW";
  case SK_ELIF_KW:
    return "ELIF_KW";
  case SK_ELSE_KW:
    return "ELSE_KW";
  case SK_LOOP_KW:
    return "LOOP_KW";
  case SK_SWITCH_KW:
    return "SWITCH_KW";
  case SK_BREAK_KW:
    return "BREAK_KW";
  case SK_CONTINUE_KW:
    return "CONTINUE_KW";
  case SK_RETURN_KW:
    return "RETURN_KW";
  case SK_DEFER_KW:
    return "DEFER_KW";
  case SK_ORELSE_KW:
    return "ORELSE_KW";

  // Effects keywords
  case SK_HANDLE_KW:
    return "HANDLE_KW";
  case SK_MASK_KW:
    return "MASK_KW";
  case SK_WITH_KW:
    return "WITH_KW";

  // Operators: logical
  case SK_AMP_AMP:
    return "AMP_AMP";
  case SK_PIPE_PIPE:
    return "PIPE_PIPE";
  case SK_BANG:
    return "BANG";

  // Operators: arithmetic
  case SK_PLUS:
    return "PLUS";
  case SK_MINUS:
    return "MINUS";
  case SK_STAR:
    return "STAR";
  case SK_STAR_STAR:
    return "STAR_STAR";
  case SK_SLASH:
    return "SLASH";
  case SK_PERCENT:
    return "PERCENT";

  // Operators: bitwise
  case SK_PIPE:
    return "PIPE";
  case SK_AMP:
    return "AMP";
  case SK_CARET:
    return "CARET";
  case SK_SHL:
    return "SHL";
  case SK_SHR:
    return "SHR";

  // Operators: relational
  case SK_EQ_EQ:
    return "EQ_EQ";
  case SK_BANG_EQ:
    return "BANG_EQ";
  case SK_LT:
    return "LT";
  case SK_LE:
    return "LE";
  case SK_GT:
    return "GT";
  case SK_GE:
    return "GE";

  // Operators: assignment
  case SK_EQ:
    return "EQ";
  case SK_PLUS_EQ:
    return "PLUS_EQ";
  case SK_MINUS_EQ:
    return "MINUS_EQ";
  case SK_STAR_EQ:
    return "STAR_EQ";
  case SK_SLASH_EQ:
    return "SLASH_EQ";
  case SK_PERCENT_EQ:
    return "PERCENT_EQ";
  case SK_PIPE_EQ:
    return "PIPE_EQ";
  case SK_AMP_EQ:
    return "AMP_EQ";
  case SK_TILDE_EQ:
    return "TILDE_EQ";
  case SK_COLON_EQ:
    return "COLON_EQ";
  case SK_PLUS_PLUS:
    return "PLUS_PLUS";
  case SK_MINUS_MINUS:
    return "MINUS_MINUS";

  // Operators: punctuation
  case SK_RARROW:
    return "RARROW";
  case SK_LARROW:
    return "LARROW";
  case SK_FATARROW:
    return "FATARROW";
  case SK_COLON:
    return "COLON";
  case SK_COLON_COLON:
    return "COLON_COLON";
  case SK_LABEL:
    return "LABEL";
  case SK_DOT:
    return "DOT";
  case SK_DOT_DOT:
    return "DOT_DOT";
  case SK_DOT_DOT_DOT:
    return "DOT_DOT_DOT";
  case SK_DOT_DOT_LT:
    return "DOT_DOT_LT";
  case SK_DOT_DOT_EQ:
    return "DOT_DOT_EQ";
  case SK_QUESTION:
    return "QUESTION";
  case SK_UNDERSCORE:
    return "UNDERSCORE";

  // Delimiters
  case SK_LPAREN:
    return "LPAREN";
  case SK_RPAREN:
    return "RPAREN";
  case SK_LBRACKET:
    return "LBRACKET";
  case SK_RBRACKET:
    return "RBRACKET";
  case SK_LBRACE:
    return "LBRACE";
  case SK_RBRACE:
    return "RBRACE";
  case SK_SEMI:
    return "SEMI";
  case SK_COMMA:
    return "COMMA";
  case SK_AT:
    return "AT";
  case SK_HASH:
    return "HASH";
  case SK_TILDE:
    return "TILDE";

  case SK_LAST_TOKEN_KIND:
    return "LAST_TOKEN_KIND";

  // ---- Node kinds ----
  case SK_FIRST_NODE_KIND:
    return "FIRST_NODE_KIND"; // also = SK_SOURCE_FILE if not careful; we keep
                              // it as the sentinel

  case SK_SOURCE_FILE:
    return "SOURCE_FILE";
  case SK_ERROR_NODE:
    return "ERROR_NODE";

  case SK_FN_DECL:
    return "FN_DECL";
  case SK_STRUCT_DECL:
    return "STRUCT_DECL";
  case SK_ENUM_DECL:
    return "ENUM_DECL";
  case SK_UNION_DECL:
    return "UNION_DECL";
  case SK_EFFECT_DECL:
    return "EFFECT_DECL";
  case SK_BIND_DECL:
    return "BIND_DECL";
  case SK_DESTRUCTURE_DECL:
    return "DESTRUCTURE_DECL";

  case SK_PARAM:
    return "PARAM";
  case SK_FIELD:
    return "FIELD";
  case SK_BIT_FIELD:
    return "BIT_FIELD";
  case SK_OP_KIND:
    return "OP_KIND";
  case SK_EFFECT_ROW_TAIL:
    return "EFFECT_ROW_TAIL";
  case SK_VARIANT:
    return "VARIANT";
  case SK_INIT_FIELD:
    return "INIT_FIELD";
  case SK_LOOP_CONTINUE:
    return "LOOP_CONTINUE";
  case SK_CAPTURE:
    return "CAPTURE";

  case SK_PARAM_LIST:
    return "PARAM_LIST";
  case SK_ARG_LIST:
    return "ARG_LIST";
  case SK_FIELD_LIST:
    return "FIELD_LIST";
  case SK_BIT_FIELD_LIST:
    return "BIT_FIELD_LIST";
  case SK_VARIANT_LIST:
    return "VARIANT_LIST";
  case SK_INIT_LIST:
    return "INIT_LIST";
  case SK_SWITCH_PATTERN_LIST:
    return "SWITCH_PATTERN_LIST";
  case SK_EFFECT_LABEL_LIST:
    return "EFFECT_LABEL_LIST";
  case SK_STMT_LIST:
    return "STMT_LIST";

  case SK_BLOCK_STMT:
    return "BLOCK_STMT";
  case SK_RETURN_STMT:
    return "RETURN_STMT";
  case SK_SWITCH_ARM:
    return "SWITCH_ARM";
  case SK_BREAK_STMT:
    return "BREAK_STMT";
  case SK_CONTINUE_STMT:
    return "CONTINUE_STMT";
  case SK_DEFER_STMT:
    return "DEFER_STMT";
  case SK_EXPR_STMT:
    return "EXPR_STMT";

  case SK_LITERAL_EXPR:
    return "LITERAL_EXPR";
  case SK_PATH_EXPR:
    return "PATH_EXPR";
  case SK_FIELD_EXPR:
    return "FIELD_EXPR";
  case SK_REF_EXPR:
    return "REF_EXPR";
  case SK_PAREN_EXPR:
    return "PAREN_EXPR";
  case SK_BIN_EXPR:
    return "BIN_EXPR";
  case SK_ASSIGN_EXPR:
    return "ASSIGN_EXPR";
  case SK_PREFIX_EXPR:
    return "PREFIX_EXPR";
  case SK_POSTFIX_EXPR:
    return "POSTFIX_EXPR";
  case SK_CALL_EXPR:
    return "CALL_EXPR";
  case SK_INDEX_EXPR:
    return "INDEX_EXPR";
  case SK_SLICE_EXPR:
    return "SLICE_EXPR";
  case SK_IF_EXPR:
    return "IF_EXPR";
  case SK_SWITCH_EXPR:
    return "SWITCH_EXPR";
  case SK_LOOP_EXPR:
    return "LOOP_EXPR";
  case SK_LAMBDA_EXPR:
    return "LAMBDA_EXPR";
  case SK_CTL_LAMBDA:
    return "CTL_LAMBDA";
  case SK_FINAL_CTL_LAMBDA:
    return "FINAL_CTL_LAMBDA";
  case SK_HANDLER_EXPR:
    return "HANDLER_EXPR";
  case SK_MASK_EXPR:
    return "MASK_EXPR";
  case SK_PRODUCT_EXPR:
    return "PRODUCT_EXPR";
  case SK_ENUM_REF_EXPR:
    return "ENUM_REF_EXPR";
  case SK_BUILTIN_EXPR:
    return "BUILTIN_EXPR";
  case SK_COMPTIME_EXPR:
    return "COMPTIME_EXPR";

  case SK_RETURN_CLAUSE:
    return "RETURN_CLAUSE";

  case SK_BIND_PAT:
    return "BIND_PAT";
  case SK_WILDCARD_PAT:
    return "WILDCARD_PAT";
  case SK_LITERAL_PAT:
    return "LITERAL_PAT";
  case SK_TUPLE_PAT:
    return "TUPLE_PAT";
  case SK_FIELD_PAT:
    return "FIELD_PAT";

  case SK_REF_TYPE:
    return "REF_TYPE";
  case SK_PATH_TYPE:
    return "PATH_TYPE";
  case SK_PTR_TYPE:
    return "PTR_TYPE";
  case SK_SLICE_TYPE:
    return "SLICE_TYPE";
  case SK_ARRAY_TYPE:
    return "ARRAY_TYPE";
  case SK_MANY_PTR_TYPE:
    return "MANY_PTR_TYPE";
  case SK_FN_TYPE:
    return "FN_TYPE";
  case SK_OPTIONAL_TYPE:
    return "OPTIONAL_TYPE";
  case SK_CONST_TYPE:
    return "CONST_TYPE";
  case SK_HANDLER_TYPE:
    return "HANDLER_TYPE";
  case SK_DISTINCT_TYPE:
    return "DISTINCT_TYPE";
  case SK_BIT_FIELD_TYPE:
    return "BIT_FIELD_TYPE";
  case SK_EFFECT_ROW_TYPE:
    return "EFFECT_ROW_TYPE";

  case SK_LAST_NODE_KIND:
    return "LAST_NODE_KIND";
  }
  return "?"; // unreachable for in-range values; only fires for raw uint16
              // cast.
}

// =====================================================================
// Range / membership classifiers
// =====================================================================
//
// Token-category classifiers are backed by a single uint16_t flag
// table indexed by SyntaxKind. Table lookup beats switch dispatch
// by ~3-5x (1 array load + 1 AND + 1 setne vs N comparisons /
// jump-table indirect branch). Hot when the parser's Pratt loop
// asks "is this a bin-op?" / "open brace?" / etc. per token.
//
// Node-category classifiers are pure range checks (contiguous enum
// values), which are O(1) and faster than table lookup — no benefit
// from the table.

enum {
  TCF_TRIVIA = 1u << 0,
  TCF_VIRTUAL_LAYOUT = 1u << 1,
  TCF_LITERAL_TOKEN = 1u << 2,
  TCF_BIN_OP = 1u << 3,
  TCF_PREFIX_OP = 1u << 4,
  TCF_POSTFIX_OP = 1u << 5,
  TCF_OPEN_BRACE = 1u << 6,
  TCF_CLOSE_BRACE = 1u << 7,
  TCF_STMT_SEP = 1u << 8,
};

// Indexed by OreSyntaxKind. Unmentioned entries default to 0
// (zero-initialized static — C standard).
static const uint16_t tok_classifier_flags[SK_LAST_TOKEN_KIND] = {
    // ---- Trivia ----
    [SK_WHITESPACE] = TCF_TRIVIA,
    [SK_NEWLINE] = TCF_TRIVIA,
    [SK_COMMENT] = TCF_TRIVIA,

    // ---- Virtual layout (both classed as VIRTUAL + their brace/sep role) ----
    [SK_VIRTUAL_LBRACE] = TCF_VIRTUAL_LAYOUT | TCF_OPEN_BRACE,
    [SK_VIRTUAL_RBRACE] = TCF_VIRTUAL_LAYOUT | TCF_CLOSE_BRACE,
    [SK_VIRTUAL_SEMI] = TCF_VIRTUAL_LAYOUT | TCF_STMT_SEP,

    // ---- Explicit braces / stmt-sep ----
    [SK_LBRACE] = TCF_OPEN_BRACE,
    [SK_RBRACE] = TCF_CLOSE_BRACE,
    [SK_SEMI] = TCF_STMT_SEP,

    // ---- Literal lexemes (not IDENT, not keyword-literals) ----
    [SK_INT_LIT] = TCF_LITERAL_TOKEN,
    [SK_FLOAT_LIT] = TCF_LITERAL_TOKEN,
    [SK_STRING_LIT] = TCF_LITERAL_TOKEN,
    [SK_BYTE_LIT] = TCF_LITERAL_TOKEN,
    [SK_ASM_LIT] = TCF_LITERAL_TOKEN,

    // ---- Operators that are BOTH binary AND prefix (position-overloaded) ----
    [SK_PLUS] = TCF_BIN_OP | TCF_PREFIX_OP,  // a+b ; +x
    [SK_MINUS] = TCF_BIN_OP | TCF_PREFIX_OP, // a-b ; -x
    [SK_AMP] = TCF_BIN_OP | TCF_PREFIX_OP,   // bitwise AND ; &x address-of
    [SK_TILDE] = TCF_BIN_OP | TCF_PREFIX_OP, // a~b XOR ; ~x bitwise NOT

    // ---- Binary-only operators ----
    // STAR is multiplication only — NOT deref. Ore deref is the postfix
    // `x^` operator (SK_CARET); pointer TYPES use `^T` prefix in
    // type-position (parsed in type-context, not via classifier).
    [SK_STAR] = TCF_BIN_OP,
    [SK_STAR_STAR] = TCF_BIN_OP,
    [SK_SLASH] = TCF_BIN_OP,
    [SK_PERCENT] = TCF_BIN_OP,
    [SK_PIPE] = TCF_BIN_OP,
    [SK_SHL] = TCF_BIN_OP,
    [SK_SHR] = TCF_BIN_OP,
    [SK_EQ_EQ] = TCF_BIN_OP,
    [SK_BANG_EQ] = TCF_BIN_OP,
    [SK_LT] = TCF_BIN_OP,
    [SK_LE] = TCF_BIN_OP,
    [SK_GT] = TCF_BIN_OP,
    [SK_GE] = TCF_BIN_OP,
    [SK_AMP_AMP] = TCF_BIN_OP,
    [SK_PIPE_PIPE] = TCF_BIN_OP,
    [SK_ORELSE_KW] = TCF_BIN_OP,

    // `..` — range expression (lo..hi yields a Range value). Used for
    // counted iteration (`loop (0..n) <i>`) and as the slice-bound
    // marker inside `[lo..hi]` (the slice parser handles that case
    // by reading lo at PREC_RANGE so the Pratt loop stops at `..`).
    [SK_DOT_DOT] = TCF_BIN_OP,
    [SK_DOT_DOT_LT] = TCF_BIN_OP,
    [SK_DOT_DOT_EQ] = TCF_BIN_OP,

    // ---- Prefix-only ----
    // BANG is logical NOT only — no postfix deerr (that AST node is
    // dead code from an earlier design).
    [SK_BANG] = TCF_PREFIX_OP,

    // ---- Postfix-only ----
    // CARET on a value is dereference (`x^`). Pointer TYPES use the
    // same character in type-prefix position (`^T`), handled by the
    // parser's type-context rules — not a classifier concern.
    [SK_CARET] = TCF_POSTFIX_OP,
    [SK_PLUS_PLUS] = TCF_POSTFIX_OP,   // x++
    [SK_MINUS_MINUS] = TCF_POSTFIX_OP, // x--
    [SK_QUESTION] = TCF_POSTFIX_OP,    // x? (denil unwrap)
};

// Lookup helper — guards out-of-range kinds (node values >= 256 would
// index past the table). Returns 0 for non-token kinds.
static inline uint16_t tcf_lookup(OreSyntaxKind k) {
  return ((uint32_t)k < SK_LAST_TOKEN_KIND) ? tok_classifier_flags[k] : 0;
}

bool ore_kind_is_token(OreSyntaxKind k) {
  return k > SK_NONE && k < SK_LAST_TOKEN_KIND;
}

bool ore_kind_is_node(OreSyntaxKind k) {
  return k >= SK_FIRST_NODE_KIND && k < SK_LAST_NODE_KIND;
}

// ---- Token-category classifiers (flag-table-backed) -----------------

bool ore_kind_is_trivia(OreSyntaxKind k) {
  return (tcf_lookup(k) & TCF_TRIVIA) != 0;
}

bool ore_kind_is_virtual_layout(OreSyntaxKind k) {
  return (tcf_lookup(k) & TCF_VIRTUAL_LAYOUT) != 0;
}

bool ore_kind_is_literal_token(OreSyntaxKind k) {
  return (tcf_lookup(k) & TCF_LITERAL_TOKEN) != 0;
}

bool ore_kind_is_bin_op_token(OreSyntaxKind k) {
  return (tcf_lookup(k) & TCF_BIN_OP) != 0;
}

bool ore_kind_is_prefix_op_token(OreSyntaxKind k) {
  return (tcf_lookup(k) & TCF_PREFIX_OP) != 0;
}

bool ore_kind_is_postfix_op_token(OreSyntaxKind k) {
  return (tcf_lookup(k) & TCF_POSTFIX_OP) != 0;
}

bool ore_kind_is_open_brace(OreSyntaxKind k) {
  return (tcf_lookup(k) & TCF_OPEN_BRACE) != 0;
}

bool ore_kind_is_close_brace(OreSyntaxKind k) {
  return (tcf_lookup(k) & TCF_CLOSE_BRACE) != 0;
}

bool ore_kind_is_stmt_sep(OreSyntaxKind k) {
  return (tcf_lookup(k) & TCF_STMT_SEP) != 0;
}

// ---- Range-check classifiers (already O(1); table would be overkill) ----

bool ore_kind_is_keyword(OreSyntaxKind k) {
  // Contiguous in enum: SK_TRUE_KW .. SK_WITH_KW.
  return k >= SK_TRUE_KW && k <= SK_WITH_KW;
}

bool ore_kind_is_assign_op_token(OreSyntaxKind k) {
  // Contiguous in enum: SK_EQ .. SK_COLON_EQ. PLUS_PLUS lives in the
  // same enum block by grammar-position grouping but is NOT an assign
  // op (it's classified as postfix via the flag table above).
  return k >= SK_EQ && k <= SK_COLON_EQ;
}

// ---- Node-category classifiers -------------------------------------

bool ore_kind_is_decl_node(OreSyntaxKind k) {
  // Contiguous: SK_FN_DECL .. SK_DESTRUCTURE_DECL.
  return k >= SK_FN_DECL && k <= SK_DESTRUCTURE_DECL;
}

bool ore_kind_is_stmt_node(OreSyntaxKind k) {
  // Contiguous: SK_BLOCK_STMT .. SK_EXPR_STMT.
  return k >= SK_BLOCK_STMT && k <= SK_EXPR_STMT;
}

bool ore_kind_is_expr_node(OreSyntaxKind k) {
  // Contiguous: SK_LITERAL_EXPR .. SK_COMPTIME_EXPR.
  return k >= SK_LITERAL_EXPR && k <= SK_COMPTIME_EXPR;
}

bool ore_kind_is_value_node(OreSyntaxKind k) {
  // Value/body position: an expression kind, plus SK_BLOCK_STMT (value-shaped
  // under Zig-strict — void unless labeled, value via `break :label v`).
  // Replaces the 4 duplicated file-local `is_expr_node` predicates that used
  // to drift apart in the ast layer.
  return ore_kind_is_expr_node(k) || k == SK_BLOCK_STMT;
}

bool ore_kind_is_lambda(OreSyntaxKind k) {
  // Contiguous: SK_LAMBDA_EXPR .. SK_FINAL_CTL_LAMBDA (fn / ctl / final-ctl).
  return k >= SK_LAMBDA_EXPR && k <= SK_FINAL_CTL_LAMBDA;
}

bool ore_kind_is_pat_node(OreSyntaxKind k) {
  // Contiguous: SK_BIND_PAT .. SK_FIELD_PAT.
  return k >= SK_BIND_PAT && k <= SK_FIELD_PAT;
}

bool ore_kind_is_type_node(OreSyntaxKind k) {
  // Contiguous: SK_REF_TYPE .. SK_EFFECT_ROW_TYPE.
  return k >= SK_REF_TYPE && k <= SK_EFFECT_ROW_TYPE;
}

bool ore_kind_is_list_node(OreSyntaxKind k) {
  // Contiguous: SK_PARAM_LIST .. SK_STMT_LIST.
  return k >= SK_PARAM_LIST && k <= SK_STMT_LIST;
}
