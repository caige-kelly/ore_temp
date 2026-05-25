// SyntaxNodePtr stability tests. Builds a tree, captures pointers,
// rebuilds the same structure into a fresh tree, and verifies the
// pointers resolve to equivalent nodes. The whole point of
// SyntaxNodePtr is to survive reparses.

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

#define DIE(...) do { fprintf(stderr, "syntax_ptr_test: " __VA_ARGS__); \
                       fprintf(stderr, "\n"); exit(1); } while (0)


// Builds: ROOT [ LIST(a) LIST(b) LIST(c) ]
static GreenNode *build_three_lists(NodeCache *cache) {
    GreenBuilder *b = green_builder_new(cache);
    green_builder_start_node(b, SK_ROOT);
    for (int i = 0; i < 3; i++) {
        green_builder_start_node(b, SK_LIST);
            green_builder_token(b, SK_LPAREN, "(", 1);
            char word[2] = {(char)('a' + i), '\0'};
            green_builder_token(b, SK_WORD, word, 1);
            green_builder_token(b, SK_RPAREN, ")", 1);
        green_builder_finish_node(b);
    }
    green_builder_finish_node(b);
    GreenNode *g = green_builder_finish(b);
    green_builder_destroy(b);
    return g;
}


// ---- Test 1: ptr roundtrip in the SAME tree -----------------------
//
// Sanity check: ptr.resolve against the tree it came from should give
// back an equivalent node.
static void test_same_tree_roundtrip(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_three_lists(cache);
    SyntaxTree *tree = syntax_tree_new(g);

    SyntaxNode *root = syntax_tree_root(tree);
    SyntaxNode *list1 = syntax_node_child(root, 1);
    if (!list1) DIE("setup: child(1) NULL");

    SyntaxNodePtr ptr = syntax_node_ptr_new(list1);
    TextRange list1_range = syntax_node_text_range(list1);
    SYN_RELEASE(list1);

    SyntaxNode *resolved = syntax_node_ptr_resolve(ptr, root);
    if (!resolved) DIE("ptr did not resolve in same tree");
    if (syntax_node_kind(resolved) != SK_LIST) DIE("resolved kind wrong");
    if (!text_range_eq(syntax_node_text_range(resolved), list1_range))
        DIE("resolved range mismatch");

    SYN_RELEASE(resolved);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    node_cache_destroy(cache);

    fprintf(stderr, "  test_same_tree_roundtrip: OK\n");
}


// ---- Test 2: ptr stability across REBUILDS ------------------------
//
// Build the tree. Capture pointers. Build the same structure into a
// fresh tree. Resolve pointers against the new root. The byte ranges
// are identical (we built the same source) so resolution should
// succeed and yield equivalent (kind, range) nodes.
//
// This is the key invariant: SyntaxNodePtr survives reparses.
static void test_resolve_across_rebuild(void) {
    NodeCache *cache = node_cache_new();

    // Build tree A.
    GreenNode *gA = build_three_lists(cache);
    SyntaxTree *treeA = syntax_tree_new(gA);
    SyntaxNode *rootA = syntax_tree_root(treeA);

    // Capture pointers to each LIST.
    SyntaxNodePtr ptrs[3];
    for (uint32_t i = 0; i < 3; i++) {
        SyntaxNode *c = syntax_node_child(rootA, i);
        if (!c) DIE("setup: child(%u) NULL in tree A", i);
        ptrs[i] = syntax_node_ptr_new(c);
        SYN_RELEASE(c);
    }
    SYN_RELEASE(rootA);
    syntax_tree_free(treeA);

    // Build tree B with the same source structure.
    GreenNode *gB = build_three_lists(cache);
    SyntaxTree *treeB = syntax_tree_new(gB);
    SyntaxNode *rootB = syntax_tree_root(treeB);

    // Resolve each ptr against tree B's root.
    for (uint32_t i = 0; i < 3; i++) {
        SyntaxNode *resolved = syntax_node_ptr_resolve(ptrs[i], rootB);
        if (!resolved) DIE("ptr[%u] did not resolve in tree B", i);
        if (syntax_node_kind(resolved) != SK_LIST)
            DIE("ptr[%u] resolved kind wrong", i);
        if (!text_range_eq(syntax_node_text_range(resolved), ptrs[i].range))
            DIE("ptr[%u] resolved range mismatch", i);
        SYN_RELEASE(resolved);
    }

    SYN_RELEASE(rootB);
    syntax_tree_free(treeB);
    node_cache_destroy(cache);

    fprintf(stderr, "  test_resolve_across_rebuild: OK\n");
}


// ---- Test 3: ptr resolution failure when target doesn't exist -----
//
// Make a ptr that DOESN'T match anything in the new tree (e.g., wrong
// kind or out-of-range). resolve should return NULL.
static void test_resolve_failure(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_three_lists(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    // Construct a ptr with a range that doesn't match any LIST.
    // The tree has LISTs at [0,3), [3,6), [6,9). Pick a range
    // that's halfway between, e.g. [2, 5).
    SyntaxNodePtr bogus = {.kind = SK_LIST, .range = {.start = 2, .length = 3}};
    SyntaxNode *resolved = syntax_node_ptr_resolve(bogus, root);
    if (resolved) DIE("bogus ptr should not resolve, got %p", (void *)resolved);

    // Wrong kind at the right range: take the first LIST's range
    // but with a non-matching kind.
    SyntaxNodePtr wrong_kind = {.kind = SK_ATOM,
                                 .range = {.start = 0, .length = 3}};
    resolved = syntax_node_ptr_resolve(wrong_kind, root);
    if (resolved) DIE("wrong-kind ptr should not resolve");

    SYN_RELEASE(root);
    syntax_tree_free(tree);
    node_cache_destroy(cache);

    fprintf(stderr, "  test_resolve_failure: OK\n");
}


// ---- Test 4: deep ptr resolution -----------------------------------
//
// Build a deeper tree and resolve a ptr to a node multiple levels
// down. Exercises the descend-and-binary-search loop.
//
// IMPORTANT: each nesting level wraps in actual delimiters so the
// ranges DIFFER at each level. Without that, three nested LISTs with
// identical (kind, range) would be ambiguous — SyntaxNodePtr's
// (kind, range) identity can't distinguish nested nodes with the
// same range. That's a fundamental property of the rowan model.
// Real-world grammars naturally produce distinct ranges via
// delimiters / siblings, so this restriction never bites in practice.
static void test_deep_resolve(void) {
    NodeCache *cache = node_cache_new();
    GreenBuilder *b = green_builder_new(cache);

    // ROOT [ ( LIST [ ( LIST [ ( LIST [ word ] ) ] ) ] ) ]
    // Each LIST is wrapped in parens, giving each level a unique range.
    green_builder_start_node(b, SK_ROOT);
        green_builder_start_node(b, SK_LIST);
            green_builder_token(b, SK_LPAREN, "(", 1);
            green_builder_start_node(b, SK_LIST);
                green_builder_token(b, SK_LPAREN, "(", 1);
                green_builder_start_node(b, SK_LIST);
                    green_builder_token(b, SK_LPAREN, "(", 1);
                    green_builder_token(b, SK_WORD, "deep", 4);
                    green_builder_token(b, SK_RPAREN, ")", 1);
                green_builder_finish_node(b);
                green_builder_token(b, SK_RPAREN, ")", 1);
            green_builder_finish_node(b);
            green_builder_token(b, SK_RPAREN, ")", 1);
        green_builder_finish_node(b);
    green_builder_finish_node(b);
    GreenNode *g = green_builder_finish(b);
    green_builder_destroy(b);

    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    // Walk to the innermost LIST.
    SyntaxNode *l1 = syntax_node_first_child(root);
    SyntaxNode *l2 = syntax_node_first_child(l1);
    SyntaxNode *l3 = syntax_node_first_child(l2);
    if (syntax_node_kind(l3) != SK_LIST) DIE("setup: deepest not LIST");
    // Verify ranges actually differ.
    TextRange r1 = syntax_node_text_range(l1);
    TextRange r2 = syntax_node_text_range(l2);
    TextRange r3 = syntax_node_text_range(l3);
    if (text_range_eq(r1, r2) || text_range_eq(r2, r3) || text_range_eq(r1, r3))
        DIE("setup: nested LISTs should have distinct ranges");
    SyntaxNodePtr ptr = syntax_node_ptr_new(l3);
    SYN_RELEASE(l1); SYN_RELEASE(l2); SYN_RELEASE(l3);

    // Resolve from the root.
    SyntaxNode *resolved = syntax_node_ptr_resolve(ptr, root);
    if (!resolved) DIE("deep ptr did not resolve");
    if (syntax_node_kind(resolved) != SK_LIST)
        DIE("deep resolve kind wrong");
    if (!text_range_eq(syntax_node_text_range(resolved), ptr.range))
        DIE("deep resolve range mismatch");

    SYN_RELEASE(resolved);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    node_cache_destroy(cache);

    fprintf(stderr, "  test_deep_resolve: OK\n");
}


int main(void) {
    fprintf(stderr, "syntax_ptr_test: starting\n");
    test_same_tree_roundtrip();
    test_resolve_across_rebuild();
    test_resolve_failure();
    test_deep_resolve();
    fprintf(stderr, "syntax_ptr_test: all PASS\n");
    return 0;
}
