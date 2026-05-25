// Mutable-tree tests for Phase 4b. Exercises:
//   - syntax_tree_new_mut + syntax_tree_root (mutable mode propagates)
//   - syntax_node_clone_for_update (immutable → mutable clone)
//   - SLL-aware navigation: two calls returning same logical child
//     yield the SAME SyntaxNode pointer (with rc bumped)
//   - Mutable-tree cascade-free (ASan-verified)
//   - Computed offsets for mutable nodes (matches green tree)
//
// Standalone — links only against src/syntax/ + src/support/.

#include "../src/syntax/syntax.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SK_ROOT = 1,
    SK_LIST,
    SK_LPAREN,
    SK_RPAREN,
    SK_WORD,
    SK_WS,
};

#define DIE(...) do { fprintf(stderr, "syntax_mutable_test: " __VA_ARGS__); \
                       fprintf(stderr, "\n"); exit(1); } while (0)


// Build:  ROOT( LIST( '(' 'a' ' ' 'b' ')' ) )
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


// ---- Test 1: immutable construction unchanged --------------------
static void test_immutable_baseline(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    if (syntax_node_is_mutable(root)) DIE("immutable root reports mutable");

    SyntaxNode *list = syntax_node_first_child(root);
    if (!list) DIE("no first child");
    if (syntax_node_is_mutable(list)) DIE("immutable child reports mutable");

    // Two calls to first_child should yield DIFFERENT pointers (immutable).
    SyntaxNode *list2 = syntax_node_first_child(root);
    if (list == list2) DIE("immutable nav should produce fresh handles");

    SYN_RELEASE(list);
    SYN_RELEASE(list2);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_immutable_baseline: OK\n");
}


// ---- Test 2: mutable tree, navigation produces same NodeData ------
static void test_mutable_navigation_aliases(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new_mut(g);
    SyntaxNode *root = syntax_tree_root(tree);

    if (!syntax_node_is_mutable(root)) DIE("mutable root reports immutable");

    SyntaxNode *list_a = syntax_node_first_child(root);
    if (!list_a) DIE("no first child");
    if (!syntax_node_is_mutable(list_a)) DIE("mutable child reports immutable");

    // Second call must return the SAME pointer (SLL hit).
    SyntaxNode *list_b = syntax_node_first_child(root);
    if (list_a != list_b) DIE("mutable nav: got distinct pointers (a=%p b=%p)",
                                (void *)list_a, (void *)list_b);

    // Release one; the other still owns its ref.
    SYN_RELEASE(list_b);
    if (syntax_node_kind(list_a) != SK_LIST) DIE("list_a kind after partial release");

    SYN_RELEASE(list_a);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_mutable_navigation_aliases: OK\n");
}


// ---- Test 3: mutable tree, token navigation also SLL-tracked -----
static void test_mutable_token_aliases(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new_mut(g);
    SyntaxNode *root = syntax_tree_root(tree);

    SyntaxNode *list = syntax_node_first_child(root);

    // list has 5 token children: ( a _ b )
    SyntaxElement e0 = syntax_node_first_child_or_token(list);
    SyntaxElement e0_again = syntax_node_first_child_or_token(list);
    if (e0.kind != SYNTAX_ELEM_TOKEN) DIE("first child not token");
    if (e0_again.kind != SYNTAX_ELEM_TOKEN) DIE("first child not token (again)");
    if (e0.token != e0_again.token)
        DIE("mutable token nav: distinct pointers (%p vs %p)",
            (void *)e0.token, (void *)e0_again.token);

    SYN_ELEM_RELEASE(e0_again);

    // Different index → different pointer.
    SyntaxElement e1 = syntax_node_child_or_token(list, 1);
    if (e1.kind != SYNTAX_ELEM_TOKEN) DIE("child 1 not token");
    if (e1.token == e0.token) DIE("different indices returned same pointer");

    SYN_ELEM_RELEASE(e0);
    SYN_ELEM_RELEASE(e1);
    SYN_RELEASE(list);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_mutable_token_aliases: OK\n");
}


// ---- Test 4: mutable tree, text ranges + offsets correct ---------
static void test_mutable_offsets(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new_mut(g);
    SyntaxNode *root = syntax_tree_root(tree);

    TextRange root_r = syntax_node_text_range(root);
    if (root_r.start != 0 || root_r.length != 5)
        DIE("root range = {%u,%u}, want {0,5}", root_r.start, root_r.length);

    SyntaxNode *list = syntax_node_first_child(root);
    TextRange list_r = syntax_node_text_range(list);
    if (list_r.start != 0 || list_r.length != 5)
        DIE("list range = {%u,%u}, want {0,5}", list_r.start, list_r.length);

    // Token 'b' is at offset 3.
    SyntaxElement e = syntax_node_child_or_token(list, 3);
    if (e.kind != SYNTAX_ELEM_TOKEN) DIE("child 3 not token");
    TextRange tr = syntax_token_text_range(e.token);
    if (tr.start != 3 || tr.length != 1)
        DIE("token 'b' range = {%u,%u}, want {3,1}", tr.start, tr.length);

    SYN_ELEM_RELEASE(e);
    SYN_RELEASE(list);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_mutable_offsets: OK\n");
}


// ---- Test 5: clone_for_update on a leaf produces mutable chain ---
static void test_clone_for_update(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    // Navigate to LIST in the immutable tree.
    SyntaxNode *list = syntax_node_first_child(root);
    if (syntax_node_is_mutable(list)) DIE("immutable list reports mutable");

    // Clone for update: returns a mutable handle to the same logical
    // position (a fresh tree underneath, sharing the green tree).
    SyntaxNode *mut_list = syntax_node_clone_for_update(list);
    if (!syntax_node_is_mutable(mut_list)) DIE("clone_for_update result not mutable");
    if (mut_list == list) DIE("clone_for_update returned same pointer (must be fresh)");

    // Same kind, same green pointer, same range.
    if (syntax_node_kind(mut_list) != syntax_node_kind(list))
        DIE("clone kind mismatch");
    if (syntax_node_green(mut_list) != syntax_node_green(list))
        DIE("clone green ptr mismatch");
    TextRange r1 = syntax_node_text_range(list);
    TextRange r2 = syntax_node_text_range(mut_list);
    if (r1.start != r2.start || r1.length != r2.length)
        DIE("clone range mismatch");

    // Parent chain: mut_list has a parent (the cloned root).
    SyntaxNode *mut_root = syntax_node_parent(mut_list);
    if (!mut_root) DIE("clone has no parent");
    if (!syntax_node_is_mutable(mut_root)) DIE("clone parent not mutable");
    if (syntax_node_parent(mut_root) != NULL) {
        DIE("clone root has a parent");  // (release leak — only on error path)
    }

    SYN_RELEASE(mut_root);
    SYN_RELEASE(mut_list);
    SYN_RELEASE(list);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_clone_for_update: OK\n");
}


// ---- Test 6: clone_for_update with deeper chain ------------------
static void test_clone_for_update_deep(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    // The LIST is at depth 1. Clone from there.
    SyntaxNode *list = syntax_node_first_child(root);
    SyntaxNode *mut_list = syntax_node_clone_for_update(list);

    // From the cloned mutable list, navigate back to root.
    SyntaxNode *mut_root = syntax_node_parent(mut_list);
    SyntaxNode *mut_root2 = syntax_node_parent(mut_list);

    // SLL: two parent calls return the SAME pointer (siblings of mut_list).
    // Actually no — mut_list IS the only child registered in mut_root's SLL,
    // and `parent` doesn't go via SLL; it just returns the cached parent ptr.
    // Both calls return the same pointer because mut_root is just refcount-bumped.
    if (mut_root != mut_root2) DIE("parent calls returned distinct pointers");

    // From mut_root, first_child should return mut_list (SLL hit).
    SyntaxNode *back_to_list = syntax_node_first_child(mut_root);
    if (back_to_list != mut_list)
        DIE("mutable nav round-trip failed: got %p, want %p",
            (void *)back_to_list, (void *)mut_list);

    SYN_RELEASE(back_to_list);
    SYN_RELEASE(mut_root2);
    SYN_RELEASE(mut_root);
    SYN_RELEASE(mut_list);
    SYN_RELEASE(list);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_clone_for_update_deep: OK\n");
}


// ---- Test 7: cascade-free leaves no leaks under ASan -------------
//
// Build a mutable tree, navigate many handles, release in scrambled
// order. ASan verifies no leaks, no double-frees, no UAF.
static void test_mutable_cascade_free(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new_mut(g);
    SyntaxNode *root = syntax_tree_root(tree);

    SyntaxNode *list = syntax_node_first_child(root);

    // Acquire all 5 token children.
    SyntaxToken *toks[5];
    for (uint32_t i = 0; i < 5; i++) {
        SyntaxElement e = syntax_node_child_or_token(list, i);
        if (e.kind != SYNTAX_ELEM_TOKEN) DIE("child %u not token", i);
        toks[i] = e.token;
    }

    // Re-fetch a couple to bump refcounts.
    SyntaxElement again = syntax_node_child_or_token(list, 2);
    if (again.token != toks[2]) DIE("SLL alias failed for token 2");

    // Release in scrambled order.
    SYN_RELEASE(toks[3]);
    SYN_RELEASE(toks[0]);
    SYN_RELEASE(again.token);
    SYN_RELEASE(toks[4]);
    SYN_RELEASE(toks[1]);
    SYN_RELEASE(toks[2]);
    SYN_RELEASE(list);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_mutable_cascade_free: OK\n");
}


int main(void) {
    fprintf(stderr, "syntax_mutable_test: starting\n");
    test_immutable_baseline();
    test_mutable_navigation_aliases();
    test_mutable_token_aliases();
    test_mutable_offsets();
    test_clone_for_update();
    test_clone_for_update_deep();
    test_mutable_cascade_free();
    fprintf(stderr, "syntax_mutable_test: all PASS\n");
    return 0;
}
