// Tests for the OreSyntaxKind enum + classifier predicates.
//
// Two properties verified:
//   1. EXHAUSTIVENESS — every value in [SK_NONE, SK_LAST_TOKEN_KIND)
//      and [SK_FIRST_NODE_KIND, SK_LAST_NODE_KIND) returns a non-"?"
//      name. Catches forgotten-case bugs in ore_syntax_kind_name.
//   2. CLASSIFIER SANITY — each ore_kind_is_* predicate has one
//      positive and one negative case.
//
// No runtime cost; ASan + UBSan clean.

#include "../src/syntax/syntax_kind.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIE(...) do { fprintf(stderr, "syntax_kind_test: " __VA_ARGS__); \
                       fprintf(stderr, "\n"); exit(1); } while (0)


// ---- Test 1: every TOKEN value has a non-"?" name ------------------
static void test_token_names_exhaustive(void) {
    int tested = 0;
    for (uint32_t v = (uint32_t)SK_NONE + 1; v < (uint32_t)SK_LAST_TOKEN_KIND; v++) {
        const char *name = ore_syntax_kind_name((OreSyntaxKind)v);
        if (!name) DIE("token kind %u: NULL name", v);
        if (strcmp(name, "?") == 0)
            DIE("token kind %u: missing name (got \"?\") — add a case to ore_syntax_kind_name",
                v);
        tested++;
    }
    fprintf(stderr, "  test_token_names_exhaustive: OK (%d tokens)\n", tested);
}


// ---- Test 2: every NODE value has a non-"?" name -------------------
static void test_node_names_exhaustive(void) {
    int tested = 0;
    for (uint32_t v = (uint32_t)SK_FIRST_NODE_KIND;
         v < (uint32_t)SK_LAST_NODE_KIND; v++) {
        const char *name = ore_syntax_kind_name((OreSyntaxKind)v);
        if (!name) DIE("node kind %u: NULL name", v);
        if (strcmp(name, "?") == 0)
            DIE("node kind %u: missing name (got \"?\") — add a case to ore_syntax_kind_name",
                v);
        tested++;
    }
    fprintf(stderr, "  test_node_names_exhaustive: OK (%d nodes)\n", tested);
}


// ---- Test 3: SK_NONE returns "NONE", out-of-range returns "?" ------
static void test_name_sentinels(void) {
    if (strcmp(ore_syntax_kind_name(SK_NONE), "NONE") != 0)
        DIE("SK_NONE should return \"NONE\"");
    // Out-of-range raw cast — should return "?" (or at least non-NULL).
    const char *n = ore_syntax_kind_name((OreSyntaxKind)0xFFFE);
    if (!n) DIE("out-of-range kind: NULL");
    if (strcmp(n, "?") != 0) DIE("out-of-range kind: expected \"?\", got %s", n);
    fprintf(stderr, "  test_name_sentinels: OK\n");
}


// ---- Test 4: classifiers — token range -----------------------------
static void test_token_node_classifiers(void) {
    if (!ore_kind_is_token(SK_PLUS)) DIE("SK_PLUS should be a token");
    if (ore_kind_is_token(SK_FN_DECL)) DIE("SK_FN_DECL should NOT be a token");
    if (!ore_kind_is_node(SK_FN_DECL)) DIE("SK_FN_DECL should be a node");
    if (ore_kind_is_node(SK_PLUS)) DIE("SK_PLUS should NOT be a node");
    if (ore_kind_is_token(SK_NONE)) DIE("SK_NONE should NOT be a token");
    if (ore_kind_is_node(SK_NONE)) DIE("SK_NONE should NOT be a node");
    fprintf(stderr, "  test_token_node_classifiers: OK\n");
}


// ---- Test 5: trivia + virtual-layout classifiers -------------------
static void test_trivia_classifiers(void) {
    if (!ore_kind_is_trivia(SK_WHITESPACE)) DIE("WHITESPACE should be trivia");
    if (!ore_kind_is_trivia(SK_NEWLINE)) DIE("NEWLINE should be trivia");
    if (!ore_kind_is_trivia(SK_COMMENT)) DIE("COMMENT should be trivia");
    if (ore_kind_is_trivia(SK_PLUS)) DIE("PLUS should NOT be trivia");
    if (ore_kind_is_trivia(SK_LEX_ERROR)) DIE("LEX_ERROR should NOT be trivia");

    if (!ore_kind_is_virtual_layout(SK_VIRTUAL_LBRACE)) DIE("VIRTUAL_LBRACE");
    if (!ore_kind_is_virtual_layout(SK_VIRTUAL_RBRACE)) DIE("VIRTUAL_RBRACE");
    if (!ore_kind_is_virtual_layout(SK_VIRTUAL_SEMI)) DIE("VIRTUAL_SEMI");
    if (ore_kind_is_virtual_layout(SK_LBRACE)) DIE("LBRACE should NOT be virtual");
    fprintf(stderr, "  test_trivia_classifiers: OK\n");
}


// ---- Test 6: keyword + literal_token classifiers -------------------
static void test_keyword_literal_classifiers(void) {
    if (!ore_kind_is_keyword(SK_FN_KW)) DIE("FN_KW is a keyword");
    if (!ore_kind_is_keyword(SK_TRUE_KW)) DIE("TRUE_KW is a keyword");
    if (!ore_kind_is_keyword(SK_WITH_KW)) DIE("WITH_KW is a keyword");
    if (ore_kind_is_keyword(SK_PLUS)) DIE("PLUS is not a keyword");
    if (ore_kind_is_keyword(SK_IDENT)) DIE("IDENT is not a keyword");

    if (!ore_kind_is_literal_token(SK_INT_LIT)) DIE("INT_LIT is a literal token");
    if (!ore_kind_is_literal_token(SK_STRING_LIT)) DIE("STRING_LIT");
    if (ore_kind_is_literal_token(SK_IDENT)) DIE("IDENT is NOT a literal token");
    if (ore_kind_is_literal_token(SK_TRUE_KW)) DIE("TRUE_KW is a keyword, not a literal token");
    fprintf(stderr, "  test_keyword_literal_classifiers: OK\n");
}


// ---- Test 7: operator classifiers ----------------------------------
static void test_operator_classifiers(void) {
    // bin op — Ore syntax: + - * / % ** & | ~ << >> == != < <= > >= && || orelse
    // (NOT caret — caret is the deref/pointer operator)
    if (!ore_kind_is_bin_op_token(SK_PLUS)) DIE("PLUS is a bin op");
    if (!ore_kind_is_bin_op_token(SK_STAR)) DIE("STAR is a bin op (multiplication)");
    if (!ore_kind_is_bin_op_token(SK_TILDE)) DIE("TILDE is a bin op (XOR)");
    if (!ore_kind_is_bin_op_token(SK_EQ_EQ)) DIE("EQ_EQ is a bin op");
    if (!ore_kind_is_bin_op_token(SK_AMP_AMP)) DIE("AMP_AMP is a bin op");
    if (!ore_kind_is_bin_op_token(SK_ORELSE_KW)) DIE("ORELSE_KW is a bin op");
    if (ore_kind_is_bin_op_token(SK_CARET)) DIE("CARET is deref, NOT a bin op");
    if (ore_kind_is_bin_op_token(SK_EQ)) DIE("EQ is assign, not bin op");
    if (ore_kind_is_bin_op_token(SK_PLUS_EQ)) DIE("PLUS_EQ is assign, not bin op");

    // assign op
    if (!ore_kind_is_assign_op_token(SK_EQ)) DIE("EQ is an assign op");
    if (!ore_kind_is_assign_op_token(SK_PLUS_EQ)) DIE("PLUS_EQ is an assign op");
    if (!ore_kind_is_assign_op_token(SK_COLON_EQ)) DIE("COLON_EQ is an assign op");
    if (ore_kind_is_assign_op_token(SK_PLUS)) DIE("PLUS is not an assign op");
    if (ore_kind_is_assign_op_token(SK_EQ_EQ)) DIE("EQ_EQ is not an assign op");
    if (ore_kind_is_assign_op_token(SK_PLUS_PLUS)) DIE("PLUS_PLUS is postfix, not assign");

    // prefix op — Ore: - ! ~ & (NOT * — star is multiplication only)
    if (!ore_kind_is_prefix_op_token(SK_MINUS)) DIE("MINUS is prefix");
    if (!ore_kind_is_prefix_op_token(SK_BANG)) DIE("BANG is prefix (logical NOT)");
    if (!ore_kind_is_prefix_op_token(SK_TILDE)) DIE("TILDE is prefix (bitwise NOT)");
    if (!ore_kind_is_prefix_op_token(SK_AMP)) DIE("AMP is prefix (address-of)");
    if (ore_kind_is_prefix_op_token(SK_STAR)) DIE("STAR is NOT prefix in Ore (multiplication only)");
    if (ore_kind_is_prefix_op_token(SK_PLUS_PLUS)) DIE("PLUS_PLUS is postfix, not prefix");

    // postfix op — Ore: ++ -- ? ^ (deref); NOT BANG (no deerr)
    if (!ore_kind_is_postfix_op_token(SK_PLUS_PLUS)) DIE("PLUS_PLUS is postfix");
    if (!ore_kind_is_postfix_op_token(SK_MINUS_MINUS)) DIE("MINUS_MINUS is postfix");
    if (!ore_kind_is_postfix_op_token(SK_QUESTION)) DIE("QUESTION is postfix (denil)");
    if (!ore_kind_is_postfix_op_token(SK_CARET)) DIE("CARET is postfix (deref `x^`)");
    if (ore_kind_is_postfix_op_token(SK_BANG)) DIE("BANG is NOT postfix in Ore (deerr is dead)");
    if (ore_kind_is_postfix_op_token(SK_MINUS)) DIE("MINUS is prefix, not postfix");
    fprintf(stderr, "  test_operator_classifiers: OK\n");
}


// ---- Test 8: brace + stmt-sep classifiers --------------------------
static void test_brace_classifiers(void) {
    if (!ore_kind_is_open_brace(SK_LBRACE)) DIE("LBRACE is open");
    if (!ore_kind_is_open_brace(SK_VIRTUAL_LBRACE)) DIE("VIRTUAL_LBRACE is open");
    if (ore_kind_is_open_brace(SK_LPAREN)) DIE("LPAREN is not a brace");
    if (ore_kind_is_open_brace(SK_RBRACE)) DIE("RBRACE is not open");

    if (!ore_kind_is_close_brace(SK_RBRACE)) DIE("RBRACE is close");
    if (!ore_kind_is_close_brace(SK_VIRTUAL_RBRACE)) DIE("VIRTUAL_RBRACE is close");

    if (!ore_kind_is_stmt_sep(SK_SEMI)) DIE("SEMI is stmt sep");
    if (!ore_kind_is_stmt_sep(SK_VIRTUAL_SEMI)) DIE("VIRTUAL_SEMI is stmt sep");
    if (ore_kind_is_stmt_sep(SK_COMMA)) DIE("COMMA is not a stmt sep");
    fprintf(stderr, "  test_brace_classifiers: OK\n");
}


// ---- Test 9: node-category classifiers -----------------------------
static void test_node_category_classifiers(void) {
    // decls
    if (!ore_kind_is_decl_node(SK_FN_DECL)) DIE("FN_DECL is decl");
    if (!ore_kind_is_decl_node(SK_STRUCT_DECL)) DIE("STRUCT_DECL is decl");
    if (!ore_kind_is_decl_node(SK_DESTRUCTURE_DECL)) DIE("DESTRUCTURE_DECL is decl");
    if (ore_kind_is_decl_node(SK_FIELD)) DIE("FIELD is a sub-decl, not top-level");
    if (ore_kind_is_decl_node(SK_BIN_EXPR)) DIE("BIN_EXPR is not a decl");

    // stmts
    if (!ore_kind_is_stmt_node(SK_BLOCK_STMT)) DIE("BLOCK_STMT is stmt");
    if (!ore_kind_is_stmt_node(SK_RETURN_STMT)) DIE("RETURN_STMT is stmt");
    if (!ore_kind_is_stmt_node(SK_EXPR_STMT)) DIE("EXPR_STMT is stmt");
    if (ore_kind_is_stmt_node(SK_BIN_EXPR)) DIE("BIN_EXPR is not a stmt");

    // exprs
    if (!ore_kind_is_expr_node(SK_BIN_EXPR)) DIE("BIN_EXPR is expr");
    if (!ore_kind_is_expr_node(SK_LITERAL_EXPR)) DIE("LITERAL_EXPR is expr");
    if (!ore_kind_is_expr_node(SK_BUILTIN_EXPR)) DIE("BUILTIN_EXPR is expr");
    if (ore_kind_is_expr_node(SK_FN_DECL)) DIE("FN_DECL is not an expr");

    // pats
    if (!ore_kind_is_pat_node(SK_BIND_PAT)) DIE("BIND_PAT is pat");
    if (!ore_kind_is_pat_node(SK_FIELD_PAT)) DIE("FIELD_PAT is pat");
    if (ore_kind_is_pat_node(SK_BIN_EXPR)) DIE("BIN_EXPR is not a pat");

    // types
    if (!ore_kind_is_type_node(SK_REF_TYPE)) DIE("REF_TYPE is type");
    if (!ore_kind_is_type_node(SK_FN_TYPE)) DIE("FN_TYPE is type");
    if (!ore_kind_is_type_node(SK_EFFECT_ROW_TYPE)) DIE("EFFECT_ROW_TYPE is type");
    if (ore_kind_is_type_node(SK_BIN_EXPR)) DIE("BIN_EXPR is not a type");

    // lists
    if (!ore_kind_is_list_node(SK_PARAM_LIST)) DIE("PARAM_LIST is list");
    if (!ore_kind_is_list_node(SK_STMT_LIST)) DIE("STMT_LIST is list");
    if (ore_kind_is_list_node(SK_BLOCK_STMT)) DIE("BLOCK_STMT is not a list");
    fprintf(stderr, "  test_node_category_classifiers: OK\n");
}


// ---- Test 10: token ↔ node ranges don't overlap --------------------
//
// SK_LAST_TOKEN_KIND must be strictly less than SK_FIRST_NODE_KIND.
// Catches any future reordering bug where someone moves a token to
// a value > 255 or a node to a value < 256.
static void test_range_separation(void) {
    if ((uint32_t)SK_LAST_TOKEN_KIND >= (uint32_t)SK_FIRST_NODE_KIND)
        DIE("token range overlaps node range");
    fprintf(stderr, "  test_range_separation: OK (tokens < %u, nodes >= %u)\n",
            (uint32_t)SK_LAST_TOKEN_KIND, (uint32_t)SK_FIRST_NODE_KIND);
}


int main(void) {
    fprintf(stderr, "syntax_kind_test: starting\n");
    test_token_names_exhaustive();
    test_node_names_exhaustive();
    test_name_sentinels();
    test_token_node_classifiers();
    test_trivia_classifiers();
    test_keyword_literal_classifiers();
    test_operator_classifiers();
    test_brace_classifiers();
    test_node_category_classifiers();
    test_range_separation();
    fprintf(stderr, "syntax_kind_test: all PASS\n");
    return 0;
}
