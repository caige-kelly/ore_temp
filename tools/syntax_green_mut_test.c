// Green-tree mutation helper tests for Phase 4c. Exercises:
//   - green_node_replace_child
//   - green_node_insert_child  (at front, middle, end)
//   - green_node_remove_child  (at front, middle, end)
//   - green_node_splice_children
//   - width / text_len recomputation
//
// All helpers are pure-functional: input nodes are unmodified, new
// nodes are RETURNS_OWNED with rc=1 retaining all (kept + inserted)
// children. ASan verifies the refcount discipline.

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

#define DIE(...) do { fprintf(stderr, "syntax_green_mut_test: " __VA_ARGS__); \
                       fprintf(stderr, "\n"); exit(1); } while (0)


// Build:  ROOT( LIST( '(' 'a' ' ' 'b' ')' ) )  → 5-child LIST
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


// Walk to inner LIST node from ROOT. Returns BORROWED GreenNode*.
static GreenNode *inner_list(GreenNode *root) {
    GreenElement c0 = green_node_child(root, 0);
    if (c0.kind != GREEN_ELEM_NODE) { DIE("inner_list: child 0 not node"); }
    return c0.node;
}


// Build a fresh token outside any tree. RETURNS_OWNED.
static GreenToken *fresh_token(NodeCache *cache, SyntaxKind k, const char *s) {
    GreenBuilder *b = green_builder_new(cache);
    green_builder_start_node(b, SK_ROOT);  // throwaway wrapper
        green_builder_token(b, k, s, (uint32_t)strlen(s));
    green_builder_finish_node(b);
    GreenNode *wrap = green_builder_finish(b);
    green_builder_destroy(b);
    GreenElement only = green_node_child(wrap, 0);
    if (only.kind != GREEN_ELEM_TOKEN) DIE("fresh_token: not a token");
    GreenToken *t = only.token;
    green_token_retain(t);     // hold onto it independently
    green_node_release(wrap);  // drops the wrapper but t stays alive
    return t;
}


// ---- Test 1: replace_child preserves count, swaps content ----------
static void test_replace_child(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *root = build_paren(cache);
    GreenNode *list = inner_list(root);

    if (green_node_num_children(list) != 5) DIE("expected 5 children");
    if (green_node_text_len(list) != 5) DIE("expected width 5");

    GreenToken *plus = fresh_token(cache, SK_PLUS, "+");
    GreenElement repl = {.kind = GREEN_ELEM_TOKEN, .token = plus};

    // Replace child 2 (the space) with '+'.
    GreenNode *new_list = green_node_replace_child(list, 2, repl);
    if (green_node_num_children(new_list) != 5) DIE("count changed");
    if (green_node_text_len(new_list) != 5) DIE("width changed");

    GreenElement c2 = green_node_child(new_list, 2);
    if (c2.kind != GREEN_ELEM_TOKEN) DIE("new c2 not token");
    if (c2.token != plus) DIE("new c2 wrong pointer");

    // Old list unmodified.
    GreenElement old_c2 = green_node_child(list, 2);
    if (green_token_kind(old_c2.token) != SK_WS) DIE("old list mutated!");

    green_node_release(new_list);
    green_token_release(plus);
    green_node_release(root);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_replace_child: OK\n");
}


// ---- Test 2: insert_child at front / middle / end -----------------
static void test_insert_child(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *root = build_paren(cache);
    GreenNode *list = inner_list(root);

    GreenToken *plus = fresh_token(cache, SK_PLUS, "+");
    GreenElement ins = {.kind = GREEN_ELEM_TOKEN, .token = plus};

    // Front
    GreenNode *front = green_node_insert_child(list, 0, ins);
    if (green_node_num_children(front) != 6) DIE("front: count = %u",
                                                  green_node_num_children(front));
    if (green_node_text_len(front) != 6) DIE("front: width = %u",
                                              green_node_text_len(front));
    GreenElement fc0 = green_node_child(front, 0);
    if (fc0.token != plus) DIE("front insert not at index 0");

    // Middle
    GreenNode *mid = green_node_insert_child(list, 3, ins);
    if (green_node_num_children(mid) != 6) DIE("mid: count");
    GreenElement mc3 = green_node_child(mid, 3);
    if (mc3.token != plus) DIE("middle insert not at index 3");

    // End
    GreenNode *end = green_node_insert_child(list, 5, ins);
    if (green_node_num_children(end) != 6) DIE("end: count");
    GreenElement ec5 = green_node_child(end, 5);
    if (ec5.token != plus) DIE("end insert not at index 5");

    green_node_release(front);
    green_node_release(mid);
    green_node_release(end);
    green_token_release(plus);
    green_node_release(root);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_insert_child: OK\n");
}


// ---- Test 3: remove_child at front / middle / end -----------------
static void test_remove_child(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *root = build_paren(cache);
    GreenNode *list = inner_list(root);

    GreenNode *rem_front = green_node_remove_child(list, 0);
    if (green_node_num_children(rem_front) != 4) DIE("front: count");
    if (green_node_text_len(rem_front) != 4) DIE("front: width");
    GreenElement nc0 = green_node_child(rem_front, 0);
    if (green_token_kind(nc0.token) != SK_WORD) DIE("front: c0 wrong kind");

    GreenNode *rem_mid = green_node_remove_child(list, 2);  // the space
    if (green_node_num_children(rem_mid) != 4) DIE("mid: count");
    if (green_node_text_len(rem_mid) != 4) DIE("mid: width");

    GreenNode *rem_end = green_node_remove_child(list, 4);
    if (green_node_num_children(rem_end) != 4) DIE("end: count");

    green_node_release(rem_front);
    green_node_release(rem_mid);
    green_node_release(rem_end);
    green_node_release(root);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_remove_child: OK\n");
}


// ---- Test 4: splice_children with delete + insert -----------------
//
// Original: '(' 'a' ' ' 'b' ')'
// Splice [1..4) with [PLUS, PLUS] → '(' '+' '+' ')'
static void test_splice_children(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *root = build_paren(cache);
    GreenNode *list = inner_list(root);

    GreenToken *plus = fresh_token(cache, SK_PLUS, "+");
    GreenElement repl[2] = {
        {.kind = GREEN_ELEM_TOKEN, .token = plus},
        {.kind = GREEN_ELEM_TOKEN, .token = plus},
    };

    GreenNode *spliced = green_node_splice_children(list, 1, 4, repl, 2);
    if (green_node_num_children(spliced) != 4) DIE("count = %u, want 4",
                                                    green_node_num_children(spliced));
    if (green_node_text_len(spliced) != 4) DIE("width = %u, want 4",
                                                green_node_text_len(spliced));

    GreenElement c0 = green_node_child(spliced, 0);
    GreenElement c1 = green_node_child(spliced, 1);
    GreenElement c2 = green_node_child(spliced, 2);
    GreenElement c3 = green_node_child(spliced, 3);
    if (green_token_kind(c0.token) != SK_LPAREN) DIE("c0 not LPAREN");
    if (c1.token != plus) DIE("c1 not plus");
    if (c2.token != plus) DIE("c2 not plus");
    if (green_token_kind(c3.token) != SK_RPAREN) DIE("c3 not RPAREN");

    green_node_release(spliced);
    green_token_release(plus);
    green_node_release(root);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_splice_children: OK\n");
}


// ---- Test 5: splice with pure deletion (count=0) ------------------
static void test_splice_deletion_only(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *root = build_paren(cache);
    GreenNode *list = inner_list(root);

    // Delete middle 3 children: a, _, b
    GreenNode *spliced = green_node_splice_children(list, 1, 4, NULL, 0);
    if (green_node_num_children(spliced) != 2) DIE("count = %u, want 2",
                                                    green_node_num_children(spliced));
    if (green_node_text_len(spliced) != 2) DIE("width = %u, want 2",
                                                green_node_text_len(spliced));

    green_node_release(spliced);
    green_node_release(root);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_splice_deletion_only: OK\n");
}


// ---- Test 6: rel_offsets are recomputed correctly ----------------
static void test_rel_offsets(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *root = build_paren(cache);
    GreenNode *list = inner_list(root);

    GreenToken *plus = fresh_token(cache, SK_PLUS, "+");
    GreenElement repl = {.kind = GREEN_ELEM_TOKEN, .token = plus};

    // Replace the space with '+'. Widths are all 1, so rel_offsets
    // should be 0, 1, 2, 3, 4 in order.
    GreenNode *new_list = green_node_replace_child(list, 2, repl);

    uint32_t expected_off = 0;
    for (uint32_t i = 0; i < 5; i++) {
        GreenElement c = green_node_child(new_list, i);
        if (c.rel_offset != expected_off)
            DIE("child %u rel_offset = %u, want %u", i, c.rel_offset, expected_off);
        expected_off += 1;  // all tokens width 1
    }

    green_node_release(new_list);
    green_token_release(plus);
    green_node_release(root);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_rel_offsets: OK\n");
}


int main(void) {
    fprintf(stderr, "syntax_green_mut_test: starting\n");
    test_replace_child();
    test_insert_child();
    test_remove_child();
    test_splice_children();
    test_splice_deletion_only();
    test_rel_offsets();
    fprintf(stderr, "syntax_green_mut_test: all PASS\n");
    return 0;
}
