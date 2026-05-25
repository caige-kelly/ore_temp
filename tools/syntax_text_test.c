// Phase 3 tests: SyntaxText lazy text view + chunks + byte_at +
// contains/find + eq_cstr. Standalone.

#include "../src/syntax/syntax.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SK_ROOT = 1, SK_LIST, SK_LPAREN, SK_RPAREN, SK_WORD, SK_WS };

#define DIE(...) do { fprintf(stderr, "syntax_text_test: " __VA_ARGS__); \
                       fprintf(stderr, "\n"); exit(1); } while (0)


// Tokens: ( a _ b )  — text "(a b)" length 5
static GreenNode *build_paren(NodeCache *cache) {
    GreenBuilder *b = green_builder_new(cache);
    green_builder_start_node(b, SK_ROOT);
        green_builder_start_node(b, SK_LIST);
            green_builder_token(b, SK_LPAREN, "(", 1);
            green_builder_token(b, SK_WORD,   "a", 1);
            green_builder_token(b, SK_WS,     " ", 1);
            green_builder_token(b, SK_WORD,   "b", 1);
            green_builder_token(b, SK_RPAREN, ")", 1);
        green_builder_finish_node(b);
    green_builder_finish_node(b);
    GreenNode *g = green_builder_finish(b);
    green_builder_destroy(b);
    return g;
}


// Tokens: ( a _ b _ c _ d _ e )  — used for slicing tests
static GreenNode *build_letters(NodeCache *cache) {
    GreenBuilder *b = green_builder_new(cache);
    green_builder_start_node(b, SK_ROOT);
        green_builder_start_node(b, SK_LIST);
            green_builder_token(b, SK_LPAREN, "(", 1);
            for (int i = 0; i < 5; i++) {
                char w[2] = {(char)('a' + i), '\0'};
                green_builder_token(b, SK_WORD, w, 1);
                if (i < 4) green_builder_token(b, SK_WS, " ", 1);
            }
            green_builder_token(b, SK_RPAREN, ")", 1);
        green_builder_finish_node(b);
    green_builder_finish_node(b);
    GreenNode *g = green_builder_finish(b);
    green_builder_destroy(b);
    return g;
}


// ---- Test 1: full subtree text materializes correctly -------------
static void test_full_text(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    SyntaxText st = syntax_text_of(root);
    if (syntax_text_len(&st) != 5) DIE("len = %u, want 5", syntax_text_len(&st));

    char buf[16];
    size_t needed = syntax_text_to_cstr(&st, buf, sizeof(buf));
    if (needed != 5) DIE("needed = %zu, want 5", needed);
    if (strcmp(buf, "(a b)") != 0) DIE("buf = %s, want (a b)", buf);

    SYN_RELEASE(root);
    syntax_tree_free(tree);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_full_text: OK\n");
}


// ---- Test 2: slice clips at token boundaries ----------------------
//
// Tree text: "(a b c d e)"  (length 11)
//   offsets:  0 1 2 3 4 5 6 7 8 9 10
//
// Slice [2, 5) should yield "a b" — wait, no. Let me recheck the
// layout: ( a _ b _ c _ d _ e )
//          0 1 2 3 4 5 6 7 8 9 10
// So [2, 7) is "_b_c_d" (offsets 2..6) → " b c " no, 5 chars:
//   offset 2: ' ' (the WS between a and b)
//   offset 3: 'b'
//   offset 4: ' ' (WS between b and c)
//   offset 5: 'c'
//   offset 6: ' '
// → " b c "
// Verify carefully.
static void test_slice(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_letters(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    SyntaxText full = syntax_text_of(root);
    if (syntax_text_len(&full) != 11) DIE("full len = %u, want 11",
                                            syntax_text_len(&full));

    // Slice middle 5 bytes: offsets [2, 7).
    SyntaxText mid = syntax_text_slice(&full, (TextRange){.start = 2, .length = 5});
    char buf[16];
    size_t needed = syntax_text_to_cstr(&mid, buf, sizeof(buf));
    if (needed != 5) DIE("mid needed = %zu, want 5", needed);
    if (strcmp(buf, " b c ") != 0) DIE("mid = '%s', want ' b c '", buf);

    // Slice that lands inside a single token: [3, 4) = "b".
    SyntaxText one = syntax_text_slice(&full, (TextRange){.start = 3, .length = 1});
    needed = syntax_text_to_cstr(&one, buf, sizeof(buf));
    if (needed != 1) DIE("one needed = %zu, want 1", needed);
    if (strcmp(buf, "b") != 0) DIE("one = '%s', want 'b'", buf);

    SYN_RELEASE(root);
    syntax_tree_free(tree);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_slice: OK\n");
}


// ---- Test 3: byte_at returns correct bytes -----------------------
static void test_byte_at(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);
    SyntaxText st = syntax_text_of(root);

    const char *expected = "(a b)";
    for (uint32_t i = 0; i < 5; i++) {
        int b = syntax_text_byte_at(&st, i);
        if (b != (unsigned char)expected[i])
            DIE("byte_at(%u) = %d, want %d", i, b, (unsigned char)expected[i]);
    }
    if (syntax_text_byte_at(&st, 5) != -1) DIE("byte_at(5) should be -1");
    if (syntax_text_byte_at(&st, 100) != -1) DIE("byte_at(100) should be -1");

    SYN_RELEASE(root);
    syntax_tree_free(tree);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_byte_at: OK\n");
}


// ---- Test 4: contains_byte / find_byte ----------------------------
static void test_find_byte(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);
    SyntaxText st = syntax_text_of(root);  // "(a b)"

    if (!syntax_text_contains_byte(&st, 'a')) DIE("should contain 'a'");
    if (!syntax_text_contains_byte(&st, ')')) DIE("should contain ')'");
    if (syntax_text_contains_byte(&st, 'Z')) DIE("should not contain 'Z'");

    if (syntax_text_find_byte(&st, '(') != 0) DIE("find '(' should be 0");
    if (syntax_text_find_byte(&st, 'a') != 1) DIE("find 'a' should be 1");
    if (syntax_text_find_byte(&st, ' ') != 2) DIE("find ' ' should be 2");
    if (syntax_text_find_byte(&st, 'b') != 3) DIE("find 'b' should be 3");
    if (syntax_text_find_byte(&st, ')') != 4) DIE("find ')' should be 4");
    if (syntax_text_find_byte(&st, 'Z') != UINT32_MAX)
        DIE("find 'Z' should be UINT32_MAX");

    SYN_RELEASE(root);
    syntax_tree_free(tree);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_find_byte: OK\n");
}


// ---- Test 5: eq_cstr -------------------------------------------------
static void test_eq_cstr(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);
    SyntaxText st = syntax_text_of(root);

    if (!syntax_text_eq_cstr(&st, "(a b)")) DIE("eq with same text failed");
    if (syntax_text_eq_cstr(&st, "(a b")) DIE("eq with prefix should fail");
    if (syntax_text_eq_cstr(&st, "(a b))")) DIE("eq with longer string should fail");
    if (syntax_text_eq_cstr(&st, "(A B)")) DIE("eq with different-case should fail");

    SYN_RELEASE(root);
    syntax_tree_free(tree);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_eq_cstr: OK\n");
}


// ---- Test 6: to_cstr truncation behavior -------------------------
static void test_to_cstr_truncate(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);
    SyntaxText st = syntax_text_of(root);  // "(a b)" = 5 chars

    char small[3];
    size_t needed = syntax_text_to_cstr(&st, small, sizeof(small));
    if (needed != 5) DIE("truncate needed = %zu, want 5", needed);
    if (strcmp(small, "(a") != 0)
        DIE("truncate buf = '%s', want '(a'", small);

    char tiny[1];
    needed = syntax_text_to_cstr(&st, tiny, sizeof(tiny));
    if (needed != 5) DIE("tiny needed = %zu, want 5", needed);
    if (tiny[0] != '\0') DIE("tiny[0] should be NUL");

    SYN_RELEASE(root);
    syntax_tree_free(tree);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_to_cstr_truncate: OK\n");
}


// ---- Test 7: slice that lands across multiple tokens -------------
//
// Tree:  "(a b c d e)"  (length 11)
// Slice [1, 6) = "a b c"  (offsets 1..5)
static void test_slice_cross_tokens(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_letters(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);
    SyntaxText full = syntax_text_of(root);

    SyntaxText cross = syntax_text_slice(&full, (TextRange){.start = 1, .length = 5});
    char buf[16];
    syntax_text_to_cstr(&cross, buf, sizeof(buf));
    if (strcmp(buf, "a b c") != 0) DIE("cross slice = '%s', want 'a b c'", buf);
    if (!syntax_text_eq_cstr(&cross, "a b c")) DIE("eq_cstr on slice failed");

    SYN_RELEASE(root);
    syntax_tree_free(tree);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_slice_cross_tokens: OK\n");
}


int main(void) {
    fprintf(stderr, "syntax_text_test: starting\n");
    test_full_text();
    test_slice();
    test_byte_at();
    test_find_byte();
    test_eq_cstr();
    test_to_cstr_truncate();
    test_slice_cross_tokens();
    fprintf(stderr, "syntax_text_test: all PASS\n");
    return 0;
}
