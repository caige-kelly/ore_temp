// Typed AST wrapper unit tests.
//
// Hand-construct small green trees, cast to typed wrappers, verify the
// accessors return the expected children. The wrappers don't allocate;
// they're zero-cost views over SyntaxNode*. All SyntaxNode/SyntaxToken
// handles returned by accessors are RETURNS_OWNED and must be released.
//
// Links: src/ast/*.c + src/syntax/*.c + src/parser/syntax_kind.c +
//        src/support/data_structure/*.c. ASan-enabled in the build rule.

#include "../src/ast/ast.h"
#include "../src/ast/ast_decl.h"
#include "../src/ast/ast_expr.h"
#include "../src/ast/ast_stmt.h"
#include "../src/ast/ast_type.h"
#include "../src/syntax/syntax_kind.h"
#include "../src/syntax/syntax.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIE(...) do { fprintf(stderr, "ast_wrappers_test: " __VA_ARGS__); \
                       fprintf(stderr, "\n"); exit(1); } while (0)

#define CHECK_TEXT(tok, expected) do {                                       \
    const char *_t = syntax_token_text(tok);                                 \
    if (strcmp(_t, (expected)) != 0)                                         \
        DIE("token text %s, expected %s", _t, (expected));                   \
} while (0)


// ---- Test 1: FnDef wrapper ------------------------------------------
//
// Tree:
//   SK_FN_DECL
//     fn IDENT(my_fn) PARAM_LIST() RARROW REF_TYPE(IDENT(i32)) BLOCK_STMT({})
static void test_fn_def(void) {
    NodeCache *cache = node_cache_new();
    GreenBuilder *b = green_builder_new(cache);

    green_builder_start_node(b, SK_FN_DECL);
        green_builder_token(b, SK_FN_KW, "fn", 2);
        green_builder_token(b, SK_IDENT, "my_fn", 5);
        green_builder_start_node(b, SK_PARAM_LIST);
            green_builder_token(b, SK_LPAREN, "(", 1);
            green_builder_token(b, SK_RPAREN, ")", 1);
        green_builder_finish_node(b);
        green_builder_token(b, SK_RARROW, "->", 2);
        green_builder_start_node(b, SK_REF_TYPE);
            green_builder_token(b, SK_IDENT, "i32", 3);
        green_builder_finish_node(b);
        green_builder_start_node(b, SK_BLOCK_STMT);
            green_builder_token(b, SK_LBRACE, "{", 1);
            green_builder_token(b, SK_RBRACE, "}", 1);
        green_builder_finish_node(b);
    green_builder_finish_node(b);

    GreenNode *root = green_builder_finish(b);
    green_builder_destroy(b);

    SyntaxTree *tree = syntax_tree_new(root);
    SyntaxNode *fn_node = syntax_tree_root(tree);

    FnDef fn;
    if (!FnDef_cast(fn_node, &fn)) DIE("FnDef_cast failed on FN_DECL");

    SyntaxToken *name = FnDef_name(&fn);
    if (!name) DIE("FnDef_name returned NULL");
    CHECK_TEXT(name, "my_fn");
    syntax_token_release(name);

    SyntaxNode *params = FnDef_params(&fn);
    if (!params) DIE("FnDef_params returned NULL");
    if (syntax_node_kind(params) != SK_PARAM_LIST)
        DIE("FnDef_params wrong kind: %u", syntax_node_kind(params));

    SyntaxNode *ret = FnDef_return_type(&fn);
    if (!ret) DIE("FnDef_return_type returned NULL");
    if (syntax_node_kind(ret) != SK_REF_TYPE)
        DIE("FnDef_return_type wrong kind: %u", syntax_node_kind(ret));
    RefType rt;
    if (!RefType_cast(ret, &rt)) DIE("RefType_cast failed on return type");
    SyntaxToken *type_name = RefType_name(&rt);
    if (!type_name) DIE("RefType_name returned NULL");
    CHECK_TEXT(type_name, "i32");
    syntax_token_release(type_name);
    syntax_node_release(ret);

    SyntaxNode *body = FnDef_body(&fn);
    if (!body) DIE("FnDef_body returned NULL");
    if (syntax_node_kind(body) != SK_BLOCK_STMT)
        DIE("FnDef_body wrong kind: %u", syntax_node_kind(body));
    syntax_node_release(body);

    // Wrong-kind cast should fail.
    FnDef bad;
    if (FnDef_cast(params, &bad)) DIE("FnDef_cast on PARAM_LIST should fail");

    syntax_node_release(params);
    syntax_node_release(fn_node);
    syntax_tree_free(tree);
    green_node_release(root);
    node_cache_destroy(cache);
    printf("test_fn_def: ok\n");
}


// ---- Test 2: BinExpr wrapper, op_kind dispatch ---------------------
//
// Tree:
//   SK_BIN_EXPR
//     LITERAL_EXPR(INT_LIT "1") PLUS LITERAL_EXPR(INT_LIT "2")
static void test_bin_expr(void) {
    NodeCache *cache = node_cache_new();
    GreenBuilder *b = green_builder_new(cache);

    green_builder_start_node(b, SK_BIN_EXPR);
        green_builder_start_node(b, SK_LITERAL_EXPR);
            green_builder_token(b, SK_INT_LIT, "1", 1);
        green_builder_finish_node(b);
        green_builder_token(b, SK_PLUS, "+", 1);
        green_builder_start_node(b, SK_LITERAL_EXPR);
            green_builder_token(b, SK_INT_LIT, "2", 1);
        green_builder_finish_node(b);
    green_builder_finish_node(b);

    GreenNode *root = green_builder_finish(b);
    green_builder_destroy(b);

    SyntaxTree *tree = syntax_tree_new(root);
    SyntaxNode *bin_node = syntax_tree_root(tree);

    BinExpr bin;
    if (!BinExpr_cast(bin_node, &bin)) DIE("BinExpr_cast failed");

    if (BinExpr_op_kind(&bin) != SK_PLUS)
        DIE("BinExpr_op_kind = %u, want SK_PLUS (%u)",
            BinExpr_op_kind(&bin), SK_PLUS);

    SyntaxNode *lhs = BinExpr_lhs(&bin);
    if (!lhs) DIE("BinExpr_lhs returned NULL");
    if (syntax_node_kind(lhs) != SK_LITERAL_EXPR)
        DIE("BinExpr_lhs wrong kind");
    Literal lit;
    if (!Literal_cast(lhs, &lit)) DIE("Literal_cast failed on lhs");
    if (Literal_kind(&lit) != SK_INT_LIT)
        DIE("Literal_kind = %u, want SK_INT_LIT (%u)",
            Literal_kind(&lit), SK_INT_LIT);
    syntax_node_release(lhs);

    SyntaxNode *rhs = BinExpr_rhs(&bin);
    if (!rhs) DIE("BinExpr_rhs returned NULL");
    if (syntax_node_kind(rhs) != SK_LITERAL_EXPR)
        DIE("BinExpr_rhs wrong kind");
    syntax_node_release(rhs);

    // No third expr — sanity check that nth_expr(2) returns NULL.
    // (Not directly accessible, but BinExpr only has 2 expr children;
    //  verify via num_children + manual inspection.)
    if (syntax_node_num_children(bin_node) != 3)
        DIE("BIN_EXPR should have 3 children (lhs, op, rhs), got %u",
            syntax_node_num_children(bin_node));

    syntax_node_release(bin_node);
    syntax_tree_free(tree);
    green_node_release(root);
    node_cache_destroy(cache);
    printf("test_bin_expr: ok\n");
}


// ---- Test 3: StructDef + Field walk --------------------------------
//
// Tree:
//   SK_STRUCT_DECL
//     struct IDENT(Point) FIELD_LIST({
//       FIELD(IDENT(x) REF_TYPE(IDENT(i32)))
//       FIELD(IDENT(y) REF_TYPE(IDENT(i32)))
//     })
static void test_struct_def(void) {
    NodeCache *cache = node_cache_new();
    GreenBuilder *b = green_builder_new(cache);

    green_builder_start_node(b, SK_STRUCT_DECL);
        green_builder_token(b, SK_STRUCT_KW, "struct", 6);
        green_builder_token(b, SK_IDENT, "Point", 5);
        green_builder_start_node(b, SK_FIELD_LIST);
            green_builder_token(b, SK_LBRACE, "{", 1);
            green_builder_start_node(b, SK_FIELD);
                green_builder_token(b, SK_IDENT, "x", 1);
                green_builder_token(b, SK_COLON, ":", 1);
                green_builder_start_node(b, SK_REF_TYPE);
                    green_builder_token(b, SK_IDENT, "i32", 3);
                green_builder_finish_node(b);
            green_builder_finish_node(b);
            green_builder_start_node(b, SK_FIELD);
                green_builder_token(b, SK_IDENT, "y", 1);
                green_builder_token(b, SK_COLON, ":", 1);
                green_builder_start_node(b, SK_REF_TYPE);
                    green_builder_token(b, SK_IDENT, "i32", 3);
                green_builder_finish_node(b);
            green_builder_finish_node(b);
            green_builder_token(b, SK_RBRACE, "}", 1);
        green_builder_finish_node(b);
    green_builder_finish_node(b);

    GreenNode *root = green_builder_finish(b);
    green_builder_destroy(b);

    SyntaxTree *tree = syntax_tree_new(root);
    SyntaxNode *st_node = syntax_tree_root(tree);

    StructDef st;
    if (!StructDef_cast(st_node, &st)) DIE("StructDef_cast failed");

    SyntaxToken *name = StructDef_name(&st);
    if (!name) DIE("StructDef_name returned NULL");
    CHECK_TEXT(name, "Point");
    syntax_token_release(name);

    SyntaxNode *fields = StructDef_fields(&st);
    if (!fields) DIE("StructDef_fields returned NULL");
    if (syntax_node_kind(fields) != SK_FIELD_LIST)
        DIE("StructDef_fields wrong kind");

    // Walk the field list, cast each FIELD child.
    SyntaxNode *f0 = ast_nth_child(fields, SK_FIELD, 0);
    if (!f0) DIE("First FIELD missing");
    Field f;
    if (!Field_cast(f0, &f)) DIE("Field_cast failed on first field");
    SyntaxToken *fname = Field_name(&f);
    if (!fname) DIE("Field_name returned NULL");
    CHECK_TEXT(fname, "x");
    syntax_token_release(fname);
    syntax_node_release(f0);

    SyntaxNode *f1 = ast_nth_child(fields, SK_FIELD, 1);
    if (!f1) DIE("Second FIELD missing");
    if (!Field_cast(f1, &f)) DIE("Field_cast failed on second field");
    fname = Field_name(&f);
    if (!fname) DIE("Field_name returned NULL");
    CHECK_TEXT(fname, "y");
    syntax_token_release(fname);
    syntax_node_release(f1);

    SyntaxNode *f2 = ast_nth_child(fields, SK_FIELD, 2);
    if (f2) DIE("Third FIELD should not exist");

    syntax_node_release(fields);
    syntax_node_release(st_node);
    syntax_tree_free(tree);
    green_node_release(root);
    node_cache_destroy(cache);
    printf("test_struct_def: ok\n");
}


// ---- Test 4: PrefixExpr op classifier + operand --------------------
//
// Tree:
//   SK_PREFIX_EXPR
//     MINUS LITERAL_EXPR(INT_LIT "7")
static void test_prefix_expr(void) {
    NodeCache *cache = node_cache_new();
    GreenBuilder *b = green_builder_new(cache);

    green_builder_start_node(b, SK_PREFIX_EXPR);
        green_builder_token(b, SK_MINUS, "-", 1);
        green_builder_start_node(b, SK_LITERAL_EXPR);
            green_builder_token(b, SK_INT_LIT, "7", 1);
        green_builder_finish_node(b);
    green_builder_finish_node(b);

    GreenNode *root = green_builder_finish(b);
    green_builder_destroy(b);

    SyntaxTree *tree = syntax_tree_new(root);
    SyntaxNode *pn = syntax_tree_root(tree);

    PrefixExpr p;
    if (!PrefixExpr_cast(pn, &p)) DIE("PrefixExpr_cast failed");

    if (PrefixExpr_op_kind(&p) != SK_MINUS)
        DIE("PrefixExpr_op_kind = %u, want SK_MINUS (%u)",
            PrefixExpr_op_kind(&p), SK_MINUS);

    SyntaxNode *operand = PrefixExpr_operand(&p);
    if (!operand) DIE("PrefixExpr_operand returned NULL");
    if (syntax_node_kind(operand) != SK_LITERAL_EXPR)
        DIE("PrefixExpr_operand wrong kind");
    syntax_node_release(operand);

    syntax_node_release(pn);
    syntax_tree_free(tree);
    green_node_release(root);
    node_cache_destroy(cache);
    printf("test_prefix_expr: ok\n");
}


// ---- Test 5: Cast against wrong kind returns false -----------------
static void test_wrong_kind_cast(void) {
    NodeCache *cache = node_cache_new();
    GreenBuilder *b = green_builder_new(cache);

    green_builder_start_node(b, SK_LITERAL_EXPR);
        green_builder_token(b, SK_INT_LIT, "42", 2);
    green_builder_finish_node(b);

    GreenNode *root = green_builder_finish(b);
    green_builder_destroy(b);

    SyntaxTree *tree = syntax_tree_new(root);
    SyntaxNode *lit = syntax_tree_root(tree);

    // The node IS a LITERAL_EXPR, so Literal_cast succeeds:
    Literal l;
    if (!Literal_cast(lit, &l)) DIE("Literal_cast should succeed on LITERAL_EXPR");
    if (Literal_kind(&l) != SK_INT_LIT) DIE("Literal_kind wrong");

    // But it is NOT a FnDef:
    FnDef bad;
    if (FnDef_cast(lit, &bad)) DIE("FnDef_cast should fail on LITERAL_EXPR");

    // And it is NOT a BinExpr:
    BinExpr bad2;
    if (BinExpr_cast(lit, &bad2)) DIE("BinExpr_cast should fail on LITERAL_EXPR");

    // NULL input fails cleanly:
    if (FnDef_cast(NULL, &bad)) DIE("FnDef_cast(NULL) should fail");

    syntax_node_release(lit);
    syntax_tree_free(tree);
    green_node_release(root);
    node_cache_destroy(cache);
    printf("test_wrong_kind_cast: ok\n");
}


int main(void) {
    test_fn_def();
    test_bin_expr();
    test_struct_def();
    test_prefix_expr();
    test_wrong_kind_cast();
    printf("ast_wrappers_test: all 5 tests passed\n");
    return 0;
}
