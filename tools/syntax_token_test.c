// Phase 1 token tests: SyntaxToken navigation, SyntaxElement iteration,
// prev_sibling, first_token/last_token, token_at_offset. Standalone.

#include "../src/syntax/syntax.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SK_ROOT = 1, SK_LIST, SK_ATOM, SK_LPAREN, SK_RPAREN, SK_WORD, SK_WS,
};

#define DIE(...) do { fprintf(stderr, "syntax_token_test: " __VA_ARGS__); \
                       fprintf(stderr, "\n"); exit(1); } while (0)


// Sample tree: ROOT [ LIST [ ( a _ b ) ] LIST [ ( c ) ] ]
// Tokens, in order: (, a, _, b, ), (, c, )
// Byte ranges:       0  1  2  3  4  5  6  7   (length 8)
static GreenNode *build_sample(NodeCache *cache) {
    GreenBuilder *b = green_builder_new(cache);
    green_builder_start_node(b, SK_ROOT);
        green_builder_start_node(b, SK_LIST);
            green_builder_token(b, SK_LPAREN, "(", 1);
            green_builder_token(b, SK_WORD,   "a", 1);
            green_builder_token(b, SK_WS,     " ", 1);
            green_builder_token(b, SK_WORD,   "b", 1);
            green_builder_token(b, SK_RPAREN, ")", 1);
        green_builder_finish_node(b);
        green_builder_start_node(b, SK_LIST);
            green_builder_token(b, SK_LPAREN, "(", 1);
            green_builder_token(b, SK_WORD,   "c", 1);
            green_builder_token(b, SK_RPAREN, ")", 1);
        green_builder_finish_node(b);
    green_builder_finish_node(b);
    GreenNode *g = green_builder_finish(b);
    green_builder_destroy(b);
    return g;
}


// ---- Test 1: first_token / last_token ------------------------------
static void test_first_last_token(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_sample(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    SyntaxToken *first = syntax_node_first_token(root);
    if (!first) DIE("first_token NULL");
    if (syntax_token_kind(first) != SK_LPAREN) DIE("first token kind wrong");
    if (strcmp(syntax_token_text(first), "(") != 0)
        DIE("first token text = %s, want \"(\"", syntax_token_text(first));
    if (syntax_token_text_range(first).start != 0) DIE("first token start wrong");

    SyntaxToken *last = syntax_node_last_token(root);
    if (!last) DIE("last_token NULL");
    if (syntax_token_kind(last) != SK_RPAREN) DIE("last token kind wrong");
    if (syntax_token_text_range(last).start != 7) DIE("last token start wrong");

    SYN_RELEASE(first);
    SYN_RELEASE(last);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_first_last_token: OK\n");
}


// ---- Test 2: child_or_token + element navigation -------------------
static void test_child_or_token(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_sample(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    // root.first_child_or_token() should be the first LIST (a node).
    SyntaxElement e = syntax_node_first_child_or_token(root);
    if (e.kind != SYNTAX_ELEM_NODE) DIE("first_child_or_token: expected NODE");
    if (syntax_node_kind(e.node) != SK_LIST) DIE("first should be LIST");

    // Drop into the first LIST. Its first child is a TOKEN (LPAREN).
    SyntaxElement child0 = syntax_node_first_child_or_token(e.node);
    if (child0.kind != SYNTAX_ELEM_TOKEN) DIE("list.child[0] should be TOKEN");
    if (syntax_token_kind(child0.token) != SK_LPAREN)
        DIE("list.child[0] should be LPAREN");

    // next_sibling_or_token of LPAREN is WORD("a")
    SyntaxElement child1 = syntax_token_next_sibling_or_token(child0.token);
    if (child1.kind != SYNTAX_ELEM_TOKEN) DIE("expected next sibling token");
    if (strcmp(syntax_token_text(child1.token), "a") != 0)
        DIE("next token text = %s, want \"a\"", syntax_token_text(child1.token));

    // last_child_or_token of the first LIST is RPAREN
    SyntaxElement last_in_list = syntax_node_last_child_or_token(e.node);
    if (last_in_list.kind != SYNTAX_ELEM_TOKEN) DIE("last should be TOKEN");
    if (syntax_token_kind(last_in_list.token) != SK_RPAREN)
        DIE("last in list should be RPAREN");

    SYN_ELEM_RELEASE(e);
    SYN_ELEM_RELEASE(child0);
    SYN_ELEM_RELEASE(child1);
    SYN_ELEM_RELEASE(last_in_list);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_child_or_token: OK\n");
}


// ---- Test 3: prev_sibling for nodes --------------------------------
static void test_node_prev_sibling(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_sample(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    SyntaxNode *l1 = syntax_node_child(root, 1);   // second LIST
    if (!l1) DIE("setup: second LIST NULL");

    SyntaxNode *l0 = syntax_node_prev_sibling(l1);
    if (!l0) DIE("prev_sibling NULL");
    if (syntax_node_kind(l0) != SK_LIST) DIE("prev sibling not LIST");
    if (syntax_node_text_range(l0).start != 0) DIE("prev list start wrong");

    SyntaxNode *l0_prev = syntax_node_prev_sibling(l0);
    if (l0_prev) DIE("prev of first child should be NULL");

    SYN_RELEASE(l0);
    SYN_RELEASE(l1);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_node_prev_sibling: OK\n");
}


// ---- Test 4: token_at_offset --------------------------------------
//
// Sample byte layout (length 8):
//   offset:  0 1 2 3 4 5 6 7
//   token:   ( a _ b ) ( c )
//
// token_at_offset(N) should return:
//   0  →  (
//   1  →  a
//   2  →  _ (whitespace)
//   3  →  b
//   4  →  ) (first close)
//   5  →  ( (second open)
//   6  →  c
//   7  →  ) (last close)
//   8  →  NULL (past end)
static void test_token_at_offset(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_sample(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    struct { uint32_t off; SyntaxKind kind; const char *text; } cases[] = {
        {0, SK_LPAREN, "("}, {1, SK_WORD, "a"}, {2, SK_WS, " "},
        {3, SK_WORD,   "b"}, {4, SK_RPAREN, ")"}, {5, SK_LPAREN, "("},
        {6, SK_WORD,   "c"}, {7, SK_RPAREN, ")"},
    };
    // Probe offsets that fall STRICTLY INSIDE a token (not at boundaries):
    // the test cases use 0..7 but our half-open layout means offset==i can
    // be a boundary. With the new BETWEEN semantics, boundary offsets
    // return BETWEEN; we probe interior offsets (offset + 0.5 conceptually
    // — but bytes are discrete, so we test offset i where i is strictly
    // inside the i-th token) by checking the LEFT side at boundaries.
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        TokenAtOffset r = syntax_token_at_offset(root, cases[i].off);
        SyntaxToken *t = NULL;
        if (r.kind == TOKEN_AT_OFFSET_SINGLE) {
            t = r.single;
        } else if (r.kind == TOKEN_AT_OFFSET_BETWEEN) {
            // Boundary: the cases[] entries are written for the
            // half-open right-biased convention. The right-side token at
            // a boundary is what they want.
            t = r.right;
            syntax_token_release(r.left);
        } else {
            DIE("token_at_offset(%u) NONE", cases[i].off);
        }
        if (syntax_token_kind(t) != cases[i].kind)
            DIE("offset %u kind = %u, want %u", cases[i].off,
                syntax_token_kind(t), cases[i].kind);
        if (strcmp(syntax_token_text(t), cases[i].text) != 0)
            DIE("offset %u text = %s, want %s", cases[i].off,
                syntax_token_text(t), cases[i].text);
        syntax_token_release(t);
    }

    // Past end → NONE
    TokenAtOffset past = syntax_token_at_offset(root, 9);
    if (past.kind != TOKEN_AT_OFFSET_NONE) DIE("token_at_offset(9) should be NONE");
    TokenAtOffset way_past = syntax_token_at_offset(root, 1000);
    if (way_past.kind != TOKEN_AT_OFFSET_NONE) DIE("token_at_offset(1000) should be NONE");

    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_token_at_offset: OK\n");
}


// ---- Test 5: token → parent → token roundtrip --------------------
//
// Walk to a token via first_token, walk back to its parent, descend
// again. Verifies the cross-type navigation works.
static void test_token_parent_roundtrip(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_sample(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    SyntaxToken *first = syntax_node_first_token(root);
    if (!first) DIE("first_token NULL");
    if (syntax_token_kind(first) != SK_LPAREN) DIE("setup: first wrong");

    SyntaxNode *parent = syntax_token_parent(first);
    if (!parent) DIE("token parent NULL");
    if (syntax_node_kind(parent) != SK_LIST) DIE("token's parent should be LIST");

    // Walk back to a token via parent.first_child_or_token() — should be
    // the same LPAREN.
    SyntaxElement back = syntax_node_first_child_or_token(parent);
    if (back.kind != SYNTAX_ELEM_TOKEN) DIE("expected token via parent");
    if (syntax_token_kind(back.token) != SK_LPAREN) DIE("roundtrip lost kind");
    if (!text_range_eq(syntax_token_text_range(back.token),
                        syntax_token_text_range(first)))
        DIE("roundtrip range mismatch");

    SYN_ELEM_RELEASE(back);
    SYN_RELEASE(parent);
    SYN_RELEASE(first);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_token_parent_roundtrip: OK\n");
}


// ---- Test 6: token sibling iteration ------------------------------
//
// From the WORD("a") token, walk through every sibling using
// next_sibling_or_token until we hit NONE. Should see _, b, ), in order.
static void test_token_sibling_walk(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_sample(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    // Get to WORD("a"): offset 1 is the boundary between '(' and 'a'.
    // We want the RIGHT side (WORD 'a').
    TokenAtOffset ra = syntax_token_at_offset(root, 1);
    SyntaxToken *a = NULL;
    if (ra.kind == TOKEN_AT_OFFSET_BETWEEN) {
        a = ra.right;
        syntax_token_release(ra.left);
    } else if (ra.kind == TOKEN_AT_OFFSET_SINGLE) {
        a = ra.single;
    } else {
        DIE("setup: token at offset 1 NONE");
    }

    SyntaxKind expected[] = {SK_WS, SK_WORD, SK_RPAREN};
    const char *expected_text[] = {" ", "b", ")"};

    SyntaxElement cur = syntax_token_next_sibling_or_token(a);
    for (int i = 0; i < 3; i++) {
        if (syntax_element_is_none(cur))
            DIE("sibling walk: expected element %d, got NONE", i);
        if (cur.kind != SYNTAX_ELEM_TOKEN)
            DIE("sibling walk: element %d not a token", i);
        if (syntax_token_kind(cur.token) != expected[i])
            DIE("sibling walk: element %d kind = %u, want %u",
                i, syntax_token_kind(cur.token), expected[i]);
        if (strcmp(syntax_token_text(cur.token), expected_text[i]) != 0)
            DIE("sibling walk: element %d text wrong", i);
        SyntaxElement next = syntax_token_next_sibling_or_token(cur.token);
        SYN_ELEM_RELEASE(cur);
        cur = next;
    }
    if (!syntax_element_is_none(cur))
        DIE("sibling walk: expected NONE after last sibling");

    SYN_RELEASE(a);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_token_sibling_walk: OK\n");
}


// ---- Test 7: empty subtree edge case ------------------------------
//
// Build a tree with an empty inner node. first_token should skip it
// and find a token deeper / further over.
static void test_empty_subtree_first_token(void) {
    NodeCache *cache = node_cache_new();
    GreenBuilder *b = green_builder_new(cache);
    green_builder_start_node(b, SK_ROOT);
        green_builder_start_node(b, SK_LIST);
            // Empty — no tokens at all in this LIST.
        green_builder_finish_node(b);
        green_builder_token(b, SK_WORD, "after", 5);
    green_builder_finish_node(b);
    GreenNode *g = green_builder_finish(b);
    green_builder_destroy(b);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    SyntaxToken *first = syntax_node_first_token(root);
    if (!first) DIE("first_token NULL despite WORD existing");
    if (strcmp(syntax_token_text(first), "after") != 0)
        DIE("first_token skipped past empty subtree wrong: got %s",
            syntax_token_text(first));

    SYN_RELEASE(first);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_empty_subtree_first_token: OK\n");
}


int main(void) {
    fprintf(stderr, "syntax_token_test: starting\n");
    test_first_last_token();
    test_child_or_token();
    test_node_prev_sibling();
    test_token_at_offset();
    test_token_parent_roundtrip();
    test_token_sibling_walk();
    test_empty_subtree_first_token();
    fprintf(stderr, "syntax_token_test: all PASS\n");
    return 0;
}
