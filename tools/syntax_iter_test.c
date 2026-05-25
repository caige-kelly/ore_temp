// Phase 2 iterator tests: ancestors, children, preorder/postorder
// with WalkEvent, descendants. Standalone.

#include "../src/syntax/syntax.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SK_ROOT = 1, SK_LIST, SK_ATOM, SK_LPAREN, SK_RPAREN, SK_WORD, SK_WS,
};

#define DIE(...) do { fprintf(stderr, "syntax_iter_test: " __VA_ARGS__); \
                       fprintf(stderr, "\n"); exit(1); } while (0)


// ROOT [ LIST [ ( a b ) ] LIST [ ( c ) ] ]
static GreenNode *build_sample(NodeCache *cache) {
    GreenBuilder *b = green_builder_new(cache);
    green_builder_start_node(b, SK_ROOT);
        green_builder_start_node(b, SK_LIST);
            green_builder_token(b, SK_LPAREN, "(", 1);
            green_builder_token(b, SK_WORD, "a", 1);
            green_builder_token(b, SK_WORD, "b", 1);
            green_builder_token(b, SK_RPAREN, ")", 1);
        green_builder_finish_node(b);
        green_builder_start_node(b, SK_LIST);
            green_builder_token(b, SK_LPAREN, "(", 1);
            green_builder_token(b, SK_WORD, "c", 1);
            green_builder_token(b, SK_RPAREN, ")", 1);
        green_builder_finish_node(b);
    green_builder_finish_node(b);
    GreenNode *g = green_builder_finish(b);
    green_builder_destroy(b);
    return g;
}


// ---- Test 1: ancestors from a leaf node walks up to root ----------
static void test_ancestors(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_sample(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    // Walk: root.child(0) is the first LIST. We iterate ancestors
    // starting from that LIST.
    SyntaxNode *first_list = syntax_node_child(root, 0);
    if (!first_list) DIE("setup: first list NULL");

    SyntaxAncestors it;
    syntax_ancestors_init(&it, first_list);

    SyntaxKind expected[] = {SK_LIST, SK_ROOT};
    int i = 0;
    SyntaxNode *n;
    while ((n = syntax_ancestors_next(&it))) {
        if (i >= 2) DIE("ancestors yielded more than expected");
        if (syntax_node_kind(n) != expected[i])
            DIE("ancestors[%d] kind = %u, want %u", i,
                syntax_node_kind(n), expected[i]);
        SYN_RELEASE(n);
        i++;
    }
    if (i != 2) DIE("ancestors yielded %d items, want 2", i);

    syntax_ancestors_free(&it);
    SYN_RELEASE(first_list);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_ancestors: OK\n");
}


// ---- Test 2: children iteration in both directions ----------------
static void test_children(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_sample(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    // Forward: should yield two LISTs.
    SyntaxChildren fwd;
    syntax_children_init(&fwd, root, SYNTAX_DIR_NEXT);
    int count_fwd = 0;
    SyntaxNode *c;
    uint32_t starts_fwd[2];
    while ((c = syntax_children_next(&fwd))) {
        if (count_fwd >= 2) DIE("children forward yielded > 2");
        if (syntax_node_kind(c) != SK_LIST)
            DIE("children forward[%d] kind wrong", count_fwd);
        starts_fwd[count_fwd] = syntax_node_text_range(c).start;
        SYN_RELEASE(c);
        count_fwd++;
    }
    if (count_fwd != 2) DIE("forward count = %d, want 2", count_fwd);
    if (starts_fwd[0] != 0 || starts_fwd[1] != 4)
        DIE("forward starts = [%u, %u], want [0, 4]",
            starts_fwd[0], starts_fwd[1]);
    syntax_children_free(&fwd);

    // Reverse: should yield the same two LISTs in reverse order.
    SyntaxChildren rev;
    syntax_children_init(&rev, root, SYNTAX_DIR_PREV);
    uint32_t starts_rev[2];
    int count_rev = 0;
    while ((c = syntax_children_next(&rev))) {
        if (count_rev >= 2) DIE("children reverse yielded > 2");
        starts_rev[count_rev] = syntax_node_text_range(c).start;
        SYN_RELEASE(c);
        count_rev++;
    }
    if (count_rev != 2) DIE("reverse count = %d, want 2", count_rev);
    if (starts_rev[0] != 4 || starts_rev[1] != 0)
        DIE("reverse starts = [%u, %u], want [4, 0]",
            starts_rev[0], starts_rev[1]);
    syntax_children_free(&rev);

    SYN_RELEASE(root);
    syntax_tree_free(tree);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_children: OK\n");
}


// ---- Test 3: children_elem yields all children (nodes + tokens) ---
static void test_children_elem(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_sample(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    // Drop into the first LIST: its children are 4 tokens.
    SyntaxNode *first = syntax_node_first_child(root);
    SyntaxChildrenElem it;
    syntax_children_elem_init(&it, first, SYNTAX_DIR_NEXT);

    int n_tokens = 0, n_nodes = 0;
    SyntaxElement e;
    while ((e = syntax_children_elem_next(&it)), !syntax_element_is_none(e)) {
        if (e.kind == SYNTAX_ELEM_TOKEN) n_tokens++;
        else n_nodes++;
        SYN_ELEM_RELEASE(e);
    }
    if (n_tokens != 4 || n_nodes != 0)
        DIE("first list elems = %d tokens, %d nodes; want 4, 0",
            n_tokens, n_nodes);

    syntax_children_elem_free(&it);

    // root's elems: 2 nodes (both LISTs), 0 tokens.
    SyntaxChildrenElem it2;
    syntax_children_elem_init(&it2, root, SYNTAX_DIR_NEXT);
    n_tokens = 0; n_nodes = 0;
    while ((e = syntax_children_elem_next(&it2)), !syntax_element_is_none(e)) {
        if (e.kind == SYNTAX_ELEM_TOKEN) n_tokens++;
        else n_nodes++;
        SYN_ELEM_RELEASE(e);
    }
    if (n_tokens != 0 || n_nodes != 2)
        DIE("root elems = %d tokens, %d nodes; want 0, 2", n_tokens, n_nodes);
    syntax_children_elem_free(&it2);

    SYN_RELEASE(first);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_children_elem: OK\n");
}


// ---- Test 4: preorder yields balanced Enter/Leave events ----------
//
// Build:  ROOT[ LIST[ ( a ) ] ]
// Expected (with Enter/Leave for every element including tokens):
//   Enter ROOT, Enter LIST, Enter (, Leave (, Enter a, Leave a,
//   Enter ), Leave ), Leave LIST, Leave ROOT
//   = 10 events; balanced.
static void test_preorder_balance(void) {
    NodeCache *cache = node_cache_new();
    GreenBuilder *b = green_builder_new(cache);
    green_builder_start_node(b, SK_ROOT);
        green_builder_start_node(b, SK_LIST);
            green_builder_token(b, SK_LPAREN, "(", 1);
            green_builder_token(b, SK_WORD, "a", 1);
            green_builder_token(b, SK_RPAREN, ")", 1);
        green_builder_finish_node(b);
    green_builder_finish_node(b);
    GreenNode *g = green_builder_finish(b);
    green_builder_destroy(b);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    SyntaxPreorder it;
    syntax_preorder_init(&it, root);

    int enters = 0, leaves = 0, total = 0;
    SyntaxWalkEvent ev;
    while ((ev = syntax_preorder_next(&it)), !syntax_walk_event_is_none(ev)) {
        if (ev.kind == SYNTAX_WALK_ENTER) enters++;
        else leaves++;
        total++;
        SYN_ELEM_RELEASE(ev.element);
        if (total > 100) DIE("preorder runaway");
    }
    if (enters != leaves)
        DIE("preorder imbalance: %d enters, %d leaves", enters, leaves);
    // 5 elements (ROOT, LIST, (, a, )) → 5 enters + 5 leaves = 10.
    if (total != 10)
        DIE("preorder total = %d, want 10", total);

    syntax_preorder_free(&it);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_preorder_balance: OK\n");
}


// ---- Test 5: descendants yields all nodes EXCEPT root ------------
//
// Build: ROOT[ LIST[ ( a ) ] LIST[ ( c ) ] ]
// Descendants (nodes only, excluding root): 2 LISTs.
static void test_descendants(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_sample(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    SyntaxDescendants it;
    syntax_descendants_init(&it, root);

    int count = 0;
    SyntaxNode *n;
    while ((n = syntax_descendants_next(&it))) {
        if (syntax_node_kind(n) != SK_LIST)
            DIE("descendant %d kind = %u, want SK_LIST", count,
                syntax_node_kind(n));
        SYN_RELEASE(n);
        count++;
    }
    if (count != 2) DIE("descendants count = %d, want 2", count);

    syntax_descendants_free(&it);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_descendants: OK\n");
}


// ---- Test 6: preorder visits every element exactly once -----------
//
// Sanity check that Enter events visit every element of the tree
// in left-to-right order.
static void test_preorder_completeness(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_sample(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    // Tree:
    //   ROOT(0..8) [
    //     LIST(0..4) [ '(' 'a' 'b' ')' ]
    //     LIST(4..8) [ '(' 'c' ')' ]
    //   ]
    // Enter events in preorder:
    //   ROOT, LIST(0..4), (, a, b, ), LIST(4..8), (, c, )
    //   = 10 enters.
    SyntaxPreorder it;
    syntax_preorder_init(&it, root);
    int enters = 0;
    SyntaxWalkEvent ev;
    while ((ev = syntax_preorder_next(&it)), !syntax_walk_event_is_none(ev)) {
        if (ev.kind == SYNTAX_WALK_ENTER) enters++;
        SYN_ELEM_RELEASE(ev.element);
    }
    if (enters != 10)
        DIE("preorder enters = %d, want 10", enters);

    syntax_preorder_free(&it);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_preorder_completeness: OK\n");
}


// ---- Test 7: early-break out of preorder (cursor cleanup ASan) ----
//
// Bail out of the iteration partway through. _free should release
// any in-progress state without leaks (ASan check).
static void test_preorder_early_break(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_sample(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    SyntaxPreorder it;
    syntax_preorder_init(&it, root);
    // Only take 3 events and then bail.
    for (int i = 0; i < 3; i++) {
        SyntaxWalkEvent ev = syntax_preorder_next(&it);
        if (syntax_walk_event_is_none(ev)) DIE("ran out early");
        SYN_ELEM_RELEASE(ev.element);
    }
    syntax_preorder_free(&it);  // ASan: should release the cursor's owned ref

    SYN_RELEASE(root);
    syntax_tree_free(tree);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_preorder_early_break: OK\n");
}


int main(void) {
    fprintf(stderr, "syntax_iter_test: starting\n");
    test_ancestors();
    test_children();
    test_children_elem();
    test_preorder_balance();
    test_descendants();
    test_preorder_completeness();
    test_preorder_early_break();
    fprintf(stderr, "syntax_iter_test: all PASS\n");
    return 0;
}
