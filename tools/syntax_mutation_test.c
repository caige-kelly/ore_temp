// Phase 4d mutation tests: detach, attach, splice_children, replace_with.
// Exercises the respine path that propagates green-tree changes up the
// ancestor chain in mutable mode. ASan-verifies refcount discipline.

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
    SK_PLUS,
};

#define DIE(...) do { fprintf(stderr, "syntax_mutation_test: " __VA_ARGS__); \
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

// Read the text of a (mutable or immutable) subtree into a buffer.
static void render(SyntaxNode *n, char *out, size_t cap) {
    SyntaxText st = syntax_text_of(n);
    syntax_text_to_cstr(&st, out, cap);
}


// ---- Test 1: detach a child shrinks parent's green ----------------
static void test_detach_child(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new_mut(g);
    SyntaxNode *root = syntax_tree_root(tree);
    SyntaxNode *list = syntax_node_first_child(root);

    // Original: "(a b)" (5 children, width 5)
    if (green_node_num_children(syntax_node_green(list)) != 5) DIE("init count");

    // Detach the WS (index 2).
    SyntaxElement ws = syntax_node_child_or_token(list, 2);
    if (ws.kind != SYNTAX_ELEM_TOKEN) DIE("c2 not token");
    syntax_token_detach(ws.token);

    // list's green should now have 4 children, width 4.
    if (green_node_num_children(syntax_node_green(list)) != 4)
        DIE("after detach: count = %u", green_node_num_children(syntax_node_green(list)));
    if (green_node_text_len(syntax_node_green(list)) != 4) DIE("after detach: width");

    // Rendered text is "(ab)".
    char buf[16];
    render(list, buf, sizeof(buf));
    if (strcmp(buf, "(ab)") != 0) DIE("rendered = '%s', want '(ab)'", buf);

    // ws is now a detached subtree root. Released here.
    SYN_ELEM_RELEASE(ws);
    SYN_RELEASE(list);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_detach_child: OK\n");
}


// ---- Test 2: attach a fresh token to a parent --------------------
static void test_attach_token(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new_mut(g);
    SyntaxNode *root = syntax_tree_root(tree);
    SyntaxNode *list = syntax_node_first_child(root);

    // Build a fresh detached mutable token: '+' wrapped in a minimal tree.
    NodeCache *aux_cache = node_cache_new();
    GreenBuilder *bb = green_builder_new(aux_cache);
    green_builder_start_node(bb, SK_ROOT);
        green_builder_token(bb, SK_PLUS, "+", 1);
    green_builder_finish_node(bb);
    GreenNode *plus_root = green_builder_finish(bb);
    green_builder_destroy(bb);

    SyntaxTree *plus_tree = syntax_tree_new_mut(plus_root);
    SyntaxNode *plus_root_node = syntax_tree_root(plus_tree);
    SyntaxElement plus_tok = syntax_node_first_child_or_token(plus_root_node);
    if (plus_tok.kind != SYNTAX_ELEM_TOKEN) DIE("plus tok wrong kind");

    syntax_token_detach(plus_tok.token);  // detach so we can re-attach

    // Attach at index 0 of list.
    syntax_node_splice_children(list, 0, 0, &plus_tok, 1);

    char buf[16];
    render(list, buf, sizeof(buf));
    if (strcmp(buf, "+(a b)") != 0) DIE("attach result = '%s', want '+(a b)'", buf);

    if (green_node_num_children(syntax_node_green(list)) != 6) DIE("attach count");

    SYN_ELEM_RELEASE(plus_tok);
    SYN_RELEASE(plus_root_node);
    syntax_tree_free(plus_tree);
    green_node_release(plus_root);
    node_cache_destroy(aux_cache);

    SYN_RELEASE(list);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_attach_token: OK\n");
}


// ---- Test 3: splice — delete N, insert M -------------------------
static void test_splice(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new_mut(g);
    SyntaxNode *root = syntax_tree_root(tree);
    SyntaxNode *list = syntax_node_first_child(root);

    // Build a detached '+' token.
    NodeCache *aux = node_cache_new();
    GreenBuilder *bb = green_builder_new(aux);
    green_builder_start_node(bb, SK_ROOT);
        green_builder_token(bb, SK_PLUS, "+", 1);
    green_builder_finish_node(bb);
    GreenNode *plus_root = green_builder_finish(bb);
    green_builder_destroy(bb);

    SyntaxTree *plus_tree = syntax_tree_new_mut(plus_root);
    SyntaxNode *plus_root_node = syntax_tree_root(plus_tree);
    SyntaxElement plus_tok = syntax_node_first_child_or_token(plus_root_node);
    syntax_token_detach(plus_tok.token);

    // Splice: delete children [1..4) (= 'a', ' ', 'b'), insert '+'.
    // Result: '(' '+' ')' = "(+)".
    syntax_node_splice_children(list, 1, 4, &plus_tok, 1);

    char buf[16];
    render(list, buf, sizeof(buf));
    if (strcmp(buf, "(+)") != 0) DIE("splice result = '%s', want '(+)'", buf);

    SYN_ELEM_RELEASE(plus_tok);
    SYN_RELEASE(plus_root_node);
    syntax_tree_free(plus_tree);
    green_node_release(plus_root);
    node_cache_destroy(aux);

    SYN_RELEASE(list);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_splice: OK\n");
}


// ---- Test 4: replace_with on a mutable subtree -------------------
//
// Build a fresh GreenNode "(c d)" and replace the LIST with it.
static void test_replace_with_mutable(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new_mut(g);
    SyntaxNode *root = syntax_tree_root(tree);
    SyntaxNode *list = syntax_node_first_child(root);

    // Build replacement: LIST( '(' 'c' ' ' 'd' ')' )
    NodeCache *aux = node_cache_new();
    GreenBuilder *bb = green_builder_new(aux);
    green_builder_start_node(bb, SK_LIST);
        green_builder_token(bb, SK_LPAREN, "(", 1);
        green_builder_token(bb, SK_WORD,   "c", 1);
        green_builder_token(bb, SK_WS,     " ", 1);
        green_builder_token(bb, SK_WORD,   "d", 1);
        green_builder_token(bb, SK_RPAREN, ")", 1);
    green_builder_finish_node(bb);
    GreenNode *new_list = green_builder_finish(bb);
    green_builder_destroy(bb);

    GreenNode *new_root = syntax_node_replace_with(list, new_list);

    // root->green should now reflect the new content.
    char buf[16];
    render(root, buf, sizeof(buf));
    if (strcmp(buf, "(c d)") != 0) DIE("replace_with result = '%s', want '(c d)'", buf);

    // new_root is the new root green (RETURNS_OWNED). Release it.
    green_node_release(new_root);
    green_node_release(new_list);  // we owned this (RETURNS_OWNED from builder); released here

    node_cache_destroy(aux);
    SYN_RELEASE(list);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_replace_with_mutable: OK\n");
}


// ---- Test 5: replace_with on the root itself ---------------------
static void test_replace_with_root(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new_mut(g);
    SyntaxNode *root = syntax_tree_root(tree);

    // Build replacement: ROOT( ... different content ... )
    NodeCache *aux = node_cache_new();
    GreenBuilder *bb = green_builder_new(aux);
    green_builder_start_node(bb, SK_ROOT);
        green_builder_token(bb, SK_WORD, "X", 1);
    green_builder_finish_node(bb);
    GreenNode *new_root_green = green_builder_finish(bb);
    green_builder_destroy(bb);

    GreenNode *out = syntax_node_replace_with(root, new_root_green);
    // For the root case in mutable mode, out is the (same) new root green.

    char buf[16];
    render(root, buf, sizeof(buf));
    if (strcmp(buf, "X") != 0) DIE("root replace = '%s', want 'X'", buf);

    green_node_release(out);
    green_node_release(new_root_green);

    node_cache_destroy(aux);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_replace_with_root: OK\n");
}


// ---- Test 6: two handles to a mutable subtree observe each other ---
//
// Get two handles to the same logical child via SLL aliasing. Mutate
// via handle A. Read via handle B — should see the new state.
static void test_handles_observe_mutations(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new_mut(g);
    SyntaxNode *root = syntax_tree_root(tree);
    SyntaxNode *list_a = syntax_node_first_child(root);
    SyntaxNode *list_b = syntax_node_first_child(root);
    if (list_a != list_b) DIE("expected SLL aliasing");

    // Mutate via list_a: detach the WS.
    SyntaxElement ws = syntax_node_child_or_token(list_a, 2);
    syntax_token_detach(ws.token);
    SYN_ELEM_RELEASE(ws);

    // Read via list_b — should see "(ab)".
    char buf[16];
    render(list_b, buf, sizeof(buf));
    if (strcmp(buf, "(ab)") != 0) DIE("via list_b: '%s', want '(ab)'", buf);

    SYN_RELEASE(list_b);
    SYN_RELEASE(list_a);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_handles_observe_mutations: OK\n");
}


// ---- Test 7: detach then attach elsewhere ------------------------
//
// Move a token from one position to another within the same parent.
static void test_detach_and_reattach(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new_mut(g);
    SyntaxNode *root = syntax_tree_root(tree);
    SyntaxNode *list = syntax_node_first_child(root);

    // Detach the WS (index 2). Hold the handle.
    SyntaxElement ws = syntax_node_child_or_token(list, 2);
    syntax_token_detach(ws.token);

    // After detach: list is "(ab)".
    char buf[16];
    render(list, buf, sizeof(buf));
    if (strcmp(buf, "(ab)") != 0) DIE("after detach: '%s'", buf);

    // Reattach at index 0.
    syntax_node_splice_children(list, 0, 0, &ws, 1);

    render(list, buf, sizeof(buf));
    if (strcmp(buf, " (ab)") != 0) DIE("after reattach: '%s', want ' (ab)'", buf);

    SYN_ELEM_RELEASE(ws);
    SYN_RELEASE(list);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_detach_and_reattach: OK\n");
}


// ---- Test 8: replace_with on immutable tree returns new green ----
static void test_replace_with_immutable(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new(g);  // IMMUTABLE
    SyntaxNode *root = syntax_tree_root(tree);
    SyntaxNode *list = syntax_node_first_child(root);

    NodeCache *aux = node_cache_new();
    GreenBuilder *bb = green_builder_new(aux);
    green_builder_start_node(bb, SK_LIST);
        green_builder_token(bb, SK_WORD, "Z", 1);
    green_builder_finish_node(bb);
    GreenNode *new_list = green_builder_finish(bb);
    green_builder_destroy(bb);

    // Build a new ROOT containing the new LIST.
    GreenNode *new_root = syntax_node_replace_with(list, new_list);

    // Original tree should be UNTOUCHED.
    char buf[16];
    render(root, buf, sizeof(buf));
    if (strcmp(buf, "(a b)") != 0) DIE("immutable: original modified! '%s'", buf);

    // Inspect new_root via a fresh tree.
    SyntaxTree *new_tree = syntax_tree_new(new_root);
    SyntaxNode *new_root_node = syntax_tree_root(new_tree);
    render(new_root_node, buf, sizeof(buf));
    if (strcmp(buf, "Z") != 0) DIE("new root: '%s', want 'Z'", buf);

    SYN_RELEASE(new_root_node);
    syntax_tree_free(new_tree);
    green_node_release(new_root);
    green_node_release(new_list);

    node_cache_destroy(aux);
    SYN_RELEASE(list);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_replace_with_immutable: OK\n");
}


int main(void) {
    fprintf(stderr, "syntax_mutation_test: starting\n");
    test_detach_child();
    test_attach_token();
    test_splice();
    test_replace_with_mutable();
    test_replace_with_root();
    test_handles_observe_mutations();
    test_detach_and_reattach();
    test_replace_with_immutable();
    fprintf(stderr, "syntax_mutation_test: all PASS\n");
    return 0;
}
