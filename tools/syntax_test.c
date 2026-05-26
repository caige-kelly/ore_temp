// Standalone test harness for src/syntax/.
//
// Verifies the green-tree primitives + builder + node cache work
// end-to-end against a tiny S-expression grammar (mirroring
// rowan/examples/s_expressions.rs). ZERO Ore dependencies — this
// binary builds and runs without any of src/db/, src/sema/, etc.
// It's the proof that the extraction contract holds.
//
// What's tested:
//   - GreenNode + GreenToken allocation, retain/release, cascade-free.
//   - NodeCache dedup (identical subtrees share GreenNode pointers).
//   - GreenBuilder start/token/finish + checkpoint (Pratt wrap).
//   - ASan-clean lifecycle (no leaks, no double-frees).

#include "../src/syntax/syntax.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Tiny syntax-kind enum specific to this test. The library treats
// these as opaque uint16_t.
enum {
    SK_ROOT = 1,
    SK_LIST,
    SK_ATOM,
    SK_LPAREN,
    SK_RPAREN,
    SK_WORD,
    SK_WS,
};

// die helper.
#define DIE(...) do { fprintf(stderr, "syntax_test: " __VA_ARGS__); \
                       fprintf(stderr, "\n"); exit(1); } while (0)


// ---- Test 1: basic build + navigate -------------------------------
//
// Build:    (a b)
// Expected: a ROOT containing one LIST containing LPAREN, WORD("a"),
//           WS, WORD("b"), RPAREN.

static void test_basic_build(void) {
    NodeCache *cache = node_cache_new();
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

    GreenNode *root = green_builder_finish(b);
    if (!root) DIE("test_basic_build: builder returned NULL");

    if (green_node_kind(root) != SK_ROOT)
        DIE("test_basic_build: root kind = %u, want %u",
            green_node_kind(root), SK_ROOT);
    if (green_node_text_len(root) != 5)
        DIE("test_basic_build: root text_len = %u, want 5",
            green_node_text_len(root));
    if (green_node_num_children(root) != 1)
        DIE("test_basic_build: root num_children = %u, want 1",
            green_node_num_children(root));

    GreenElement list_elem = green_node_child(root, 0);
    if (list_elem.kind != GREEN_ELEM_NODE || !list_elem.node)
        DIE("test_basic_build: root.child[0] is not a node");
    if (green_node_kind(list_elem.node) != SK_LIST)
        DIE("test_basic_build: list kind = %u, want %u",
            green_node_kind(list_elem.node), SK_LIST);
    if (green_node_num_children(list_elem.node) != 5)
        DIE("test_basic_build: list num_children = %u, want 5",
            green_node_num_children(list_elem.node));

    // Spot-check the second child (a WORD token).
    GreenElement a_elem = green_node_child(list_elem.node, 1);
    if (a_elem.kind != GREEN_ELEM_TOKEN || !a_elem.token)
        DIE("test_basic_build: list.child[1] is not a token");
    if (green_token_kind(a_elem.token) != SK_WORD)
        DIE("test_basic_build: WORD kind wrong");
    if (strcmp(green_token_text(a_elem.token), "a") != 0)
        DIE("test_basic_build: WORD text = %s, want \"a\"",
            green_token_text(a_elem.token));

    // Release the root (cascades through every child).
    SYN_RELEASE(root);
    green_builder_destroy(b);
    node_cache_destroy(cache);

    fprintf(stderr, "  test_basic_build: OK\n");
}


// ---- Test 2: token dedup -----------------------------------------
//
// Build the same token in two different places. The library should
// intern it and both call sites get the same pointer.

static void test_token_dedup(void) {
    NodeCache *cache = node_cache_new();
    GreenBuilder *b = green_builder_new(cache);

    green_builder_start_node(b, SK_ROOT);
        green_builder_token(b, SK_WORD, "foo", 3);
        green_builder_token(b, SK_WORD, "foo", 3);
    green_builder_finish_node(b);

    GreenNode *root = green_builder_finish(b);
    GreenElement c0 = green_node_child(root, 0);
    GreenElement c1 = green_node_child(root, 1);
    if (c0.token != c1.token)
        DIE("test_token_dedup: two `foo` tokens should be pointer-equal");

    // Distinct text → distinct token.
    green_builder_start_node(b, SK_ROOT);
        green_builder_token(b, SK_WORD, "foo", 3);
        green_builder_token(b, SK_WORD, "bar", 3);
    green_builder_finish_node(b);
    GreenNode *root2 = green_builder_finish(b);
    GreenElement d0 = green_node_child(root2, 0);
    GreenElement d1 = green_node_child(root2, 1);
    if (d0.token == d1.token)
        DIE("test_token_dedup: `foo` and `bar` should be distinct");
    // The earlier `foo` and this one should still match.
    if (d0.token != c0.token)
        DIE("test_token_dedup: `foo` across two builds should be pointer-equal "
            "(cache reused)");

    SYN_RELEASE(root);
    SYN_RELEASE(root2);
    green_builder_destroy(b);
    node_cache_destroy(cache);

    fprintf(stderr, "  test_token_dedup: OK\n");
}


// ---- Test 3: node dedup (small subtree dedup) ---------------------
//
// Build (a) twice, in different positions. Both LIST subtrees should
// be the same GreenNode pointer (rowan-style hash-cons on ≤3 children).

static void test_node_dedup(void) {
    NodeCache *cache = node_cache_new();
    GreenBuilder *b = green_builder_new(cache);

    // Outer ROOT containing two identical LISTs: (a) (a)
    green_builder_start_node(b, SK_ROOT);
        // First (a)
        green_builder_start_node(b, SK_LIST);
            green_builder_token(b, SK_LPAREN, "(", 1);
            green_builder_token(b, SK_WORD,   "a", 1);
            green_builder_token(b, SK_RPAREN, ")", 1);
        green_builder_finish_node(b);
        // Second (a) — same kind, same children pointers (tokens already
        // interned) → should dedup to the same node.
        green_builder_start_node(b, SK_LIST);
            green_builder_token(b, SK_LPAREN, "(", 1);
            green_builder_token(b, SK_WORD,   "a", 1);
            green_builder_token(b, SK_RPAREN, ")", 1);
        green_builder_finish_node(b);
    green_builder_finish_node(b);

    GreenNode *root = green_builder_finish(b);
    GreenElement c0 = green_node_child(root, 0);
    GreenElement c1 = green_node_child(root, 1);
    if (c0.kind != GREEN_ELEM_NODE || c1.kind != GREEN_ELEM_NODE)
        DIE("test_node_dedup: children must be nodes");
    if (c0.node != c1.node)
        DIE("test_node_dedup: identical (a) subtrees should dedup to one "
            "GreenNode pointer (got %p vs %p)", (void *)c0.node, (void *)c1.node);

    SYN_RELEASE(root);
    green_builder_destroy(b);
    node_cache_destroy(cache);

    fprintf(stderr, "  test_node_dedup: OK\n");
}


// ---- Test 4: checkpoint / start_node_at --------------------------
//
// Mirrors rowan's math.rs Pratt pattern: parse an atom, then wrap
// retroactively into a binop.

static void test_checkpoint(void) {
    NodeCache *cache = node_cache_new();
    GreenBuilder *b = green_builder_new(cache);

    green_builder_start_node(b, SK_ROOT);
        Checkpoint cp = green_builder_checkpoint(b);
        green_builder_token(b, SK_WORD, "x", 1);
        // Decided: wrap the just-parsed `x` into an ATOM after the fact.
        green_builder_start_node_at(b, cp, SK_ATOM);
        green_builder_finish_node(b);
    green_builder_finish_node(b);

    GreenNode *root = green_builder_finish(b);
    if (green_node_num_children(root) != 1)
        DIE("test_checkpoint: root should have 1 child (the wrapped ATOM)");
    GreenElement atom = green_node_child(root, 0);
    if (atom.kind != GREEN_ELEM_NODE || green_node_kind(atom.node) != SK_ATOM)
        DIE("test_checkpoint: wrapped child is not ATOM");
    if (green_node_num_children(atom.node) != 1)
        DIE("test_checkpoint: ATOM should contain 1 token");

    SYN_RELEASE(root);
    green_builder_destroy(b);
    node_cache_destroy(cache);

    fprintf(stderr, "  test_checkpoint: OK\n");
}


// ---- Test 5: refcount lifecycle (ASan check) ----------------------
//
// Stress-test retain/release. Builds a tree, retains the root, then
// releases twice. ASan + LeakSanitizer should be clean.

static void test_refcount_lifecycle(void) {
    NodeCache *cache = node_cache_new();
    GreenBuilder *b = green_builder_new(cache);

    green_builder_start_node(b, SK_ROOT);
        green_builder_token(b, SK_WORD, "alpha", 5);
    green_builder_finish_node(b);

    // After finish, the node is held by both the cache (1 ref) and the
    // caller (1 ref returned to us). Stress the retain/release path
    // with a second handle so every ref gets a matching release.
    GreenNode *root = green_builder_finish(b);
    GreenNode *root2 = root;
    green_node_retain(root);   // +1 for root2; both handles valid
    SYN_RELEASE(root);          // release one caller ref, root = NULL
    if (root != NULL) DIE("test_refcount_lifecycle: SYN_RELEASE didn't null pointer");

    green_builder_destroy(b);
    SYN_RELEASE(root2);          // drop the last caller ref
    node_cache_destroy(cache);   // releases the cache's ref, frees the tree

    fprintf(stderr, "  test_refcount_lifecycle: OK\n");
}


int main(void) {
    fprintf(stderr, "syntax_test: starting\n");
    test_basic_build();
    test_token_dedup();
    test_node_dedup();
    test_checkpoint();
    test_refcount_lifecycle();
    fprintf(stderr, "syntax_test: all PASS\n");
    return 0;
}
