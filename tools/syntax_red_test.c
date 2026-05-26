// Red-tree navigation tests. Exercises SyntaxTree + SyntaxNode +
// parent/child/sibling navigation + cascade-free under ASan.
//
// Standalone: links only against src/syntax/ + src/support/. Proves
// the extraction contract.

#include "../src/syntax/syntax.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SK_ROOT = 1,
    SK_LIST,
    SK_ATOM,
    SK_LPAREN,
    SK_RPAREN,
    SK_WORD,
    SK_WS,
};

#define DIE(...) do { fprintf(stderr, "syntax_red_test: " __VA_ARGS__); \
                       fprintf(stderr, "\n"); exit(1); } while (0)


// Helper: build the tree
//   ROOT
//     LIST [(, a, _, b, )]
//     LIST [(, c, )]
//
// Used by multiple tests.
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


// ---- Test 1: root + first_child + next_sibling ---------------------
static void test_root_walk(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_sample(cache);
    SyntaxTree *tree = syntax_tree_new(g);

    SyntaxNode *root = syntax_tree_root(tree);
    if (syntax_node_kind(root) != SK_ROOT) DIE("root kind wrong");

    TextRange rr = syntax_node_text_range(root);
    if (rr.start != 0) DIE("root start = %u, want 0", rr.start);
    if (rr.length != 8) DIE("root length = %u, want 8 (1+1+1+1+1+1+1+1)", rr.length);

    SyntaxNode *first = syntax_node_first_child(root);
    if (!first) DIE("first_child returned NULL");
    if (syntax_node_kind(first) != SK_LIST) DIE("first child not LIST");
    TextRange fr = syntax_node_text_range(first);
    if (fr.start != 0) DIE("first list start = %u, want 0", fr.start);
    if (fr.length != 5) DIE("first list length = %u, want 5", fr.length);

    SyntaxNode *second = syntax_node_next_sibling(first);
    if (!second) DIE("next_sibling returned NULL");
    if (syntax_node_kind(second) != SK_LIST) DIE("second child not LIST");
    TextRange sr = syntax_node_text_range(second);
    if (sr.start != 5) DIE("second list start = %u, want 5", sr.start);
    if (sr.length != 3) DIE("second list length = %u, want 3", sr.length);

    SyntaxNode *third = syntax_node_next_sibling(second);
    if (third) DIE("next_sibling beyond last should be NULL");

    SYN_RELEASE(third);
    SYN_RELEASE(second);
    SYN_RELEASE(first);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);

    fprintf(stderr, "  test_root_walk: OK\n");
}


// ---- Test 2: parent walk -------------------------------------------
static void test_parent_walk(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_sample(cache);
    SyntaxTree *tree = syntax_tree_new(g);

    SyntaxNode *root = syntax_tree_root(tree);
    SyntaxNode *first = syntax_node_first_child(root);

    // first.parent() should give us back something equivalent to root.
    SyntaxNode *first_parent = syntax_node_parent(first);
    if (!first_parent) DIE("first.parent() returned NULL");
    if (syntax_node_kind(first_parent) != SK_ROOT)
        DIE("first.parent() kind = %u, want SK_ROOT", syntax_node_kind(first_parent));
    if (!text_range_eq(syntax_node_text_range(first_parent),
                        syntax_node_text_range(root)))
        DIE("first.parent() range mismatch with root");

    // root.parent() should be NULL.
    SyntaxNode *root_parent = syntax_node_parent(root);
    if (root_parent) DIE("root.parent() should be NULL");

    SYN_RELEASE(first_parent);
    SYN_RELEASE(first);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);

    fprintf(stderr, "  test_parent_walk: OK\n");
}


// ---- Test 3: child access by index ---------------------------------
//
// Verifies that syntax_node_child(i) returns NULL for token children
// (LPAREN etc.) but returns a SyntaxNode for node children (the LIST
// at i=0 and i=1 inside the ROOT).
static void test_child_by_index(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_sample(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    if (syntax_node_num_children(root) != 2)
        DIE("root num_children = %u, want 2", syntax_node_num_children(root));

    // child(0): the first LIST.
    SyntaxNode *c0 = syntax_node_child(root, 0);
    if (!c0) DIE("child(0) NULL");
    if (syntax_node_kind(c0) != SK_LIST) DIE("child(0) kind wrong");

    // child(1): the second LIST.
    SyntaxNode *c1 = syntax_node_child(root, 1);
    if (!c1) DIE("child(1) NULL");
    if (syntax_node_kind(c1) != SK_LIST) DIE("child(1) kind wrong");

    // child(2): out of range.
    SyntaxNode *c2 = syntax_node_child(root, 2);
    if (c2) DIE("child(2) should be NULL");

    // Now drop into LIST [( a _ b )] and check that child(0) returns
    // NULL (LPAREN is a token).
    SyntaxNode *list_child0 = syntax_node_child(c0, 0);
    if (list_child0) DIE("list.child(0) should be NULL — LPAREN is a token");

    SYN_RELEASE(c0);
    SYN_RELEASE(c1);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);

    fprintf(stderr, "  test_child_by_index: OK\n");
}


// ---- Test 4: deep nesting cascade-free under ASan ------------------
//
// Build a deeply-nested tree, navigate to the deepest descendant,
// release everything. ASan + LeakSanitizer should be clean. Catches
// refcount discipline bugs.
static void test_deep_cascade(void) {
    NodeCache *cache = node_cache_new();

    // Build: ROOT { LIST { LIST { LIST { ... { LIST { WORD } } } } } }
    GreenBuilder *b = green_builder_new(cache);
    const int DEPTH = 16;
    green_builder_start_node(b, SK_ROOT);
    for (int i = 0; i < DEPTH; i++) green_builder_start_node(b, SK_LIST);
    green_builder_token(b, SK_WORD, "x", 1);
    for (int i = 0; i < DEPTH; i++) green_builder_finish_node(b);
    green_builder_finish_node(b);
    GreenNode *g = green_builder_finish(b);
    green_builder_destroy(b);
    SyntaxTree *tree = syntax_tree_new(g);

    // Walk to the deepest LIST via repeated first_child.
    SyntaxNode *root = syntax_tree_root(tree);
    SyntaxNode *cur = syntax_node_first_child(root);
    while (cur && syntax_node_num_children(cur) > 0) {
        SyntaxNode *next = syntax_node_first_child(cur);
        if (!next) break;
        SYN_RELEASE(cur);
        cur = next;
    }
    // Drop everything. Cascade should walk all the way up to root.
    SYN_RELEASE(cur);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);

    fprintf(stderr, "  test_deep_cascade: OK\n");
}


int main(void) {
    fprintf(stderr, "syntax_red_test: starting\n");
    test_root_walk();
    test_parent_walk();
    test_child_by_index();
    test_deep_cascade();
    fprintf(stderr, "syntax_red_test: all PASS\n");
    return 0;
}
