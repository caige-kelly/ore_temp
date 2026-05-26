// Phase 4f tests — verifies all 12 new public API functions added to
// close the rowan-parity gap. ASan-verified.

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
    SK_ATOM,
};

#define DIE(...) do { fprintf(stderr, "syntax_extras_test: " __VA_ARGS__); \
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


// ---- Test 1: clone_subtree -----------------------------------------
static void test_clone_subtree(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);
    SyntaxNode *list = syntax_node_first_child(root);

    SyntaxNode *list_subtree = syntax_node_clone_subtree(list);

    // New root: parent NULL.
    if (syntax_node_parent(list_subtree) != NULL) DIE("clone has a parent");
    // Same kind and same green pointer.
    if (syntax_node_kind(list_subtree) != syntax_node_kind(list)) DIE("kind mismatch");
    if (syntax_node_green(list_subtree) != syntax_node_green(list)) DIE("green ptr mismatch");
    // Offset is 0 in the new tree.
    TextRange r = syntax_node_text_range(list_subtree);
    if (r.start != 0 || r.length != 5)
        DIE("clone range = {%u,%u}, want {0,5}", r.start, r.length);
    if (syntax_node_is_mutable(list_subtree)) DIE("clone should be immutable");

    SYN_RELEASE(list_subtree);
    SYN_RELEASE(list);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_clone_subtree: OK\n");
}


// ---- Test 2: by_kind matchers --------------------------------------
static void test_by_kind_matchers(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);
    SyntaxNode *list = syntax_node_first_child(root);

    // first_child_or_token_by_kind: LPAREN at index 0.
    SyntaxElement lp = syntax_node_first_child_or_token_by_kind(list, SK_LPAREN);
    if (lp.kind != SYNTAX_ELEM_TOKEN) DIE("LPAREN matcher kind");
    if (syntax_token_kind(lp.token) != SK_LPAREN) DIE("LPAREN matcher wrong");
    SYN_ELEM_RELEASE(lp);

    // first_child_or_token_by_kind: WORD at index 1 (first WORD).
    SyntaxElement w = syntax_node_first_child_or_token_by_kind(list, SK_WORD);
    if (w.kind != SYNTAX_ELEM_TOKEN) DIE("WORD matcher kind");
    if (strcmp(syntax_token_text(w.token), "a") != 0) DIE("first WORD should be 'a'");

    // next_sibling_or_token_by_kind from 'a' WORD → next WORD is 'b'.
    SyntaxElement w2 = syntax_token_next_sibling_or_token_by_kind(w.token, SK_WORD);
    if (w2.kind != SYNTAX_ELEM_TOKEN) DIE("next WORD matcher kind");
    if (strcmp(syntax_token_text(w2.token), "b") != 0) DIE("next WORD should be 'b'");
    SYN_ELEM_RELEASE(w2);

    // Absence: searching for SK_PLUS returns NONE.
    SyntaxElement none = syntax_node_first_child_or_token_by_kind(list, SK_PLUS);
    if (!syntax_element_is_none(none)) DIE("expected NONE for absent kind");

    // first_child_by_kind returns NULL for token-only LIST (no node children).
    SyntaxNode *no_node = syntax_node_first_child_by_kind(list, SK_WORD);
    if (no_node != NULL) DIE("first_child_by_kind should reject tokens");

    SYN_ELEM_RELEASE(w);
    SYN_RELEASE(list);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_by_kind_matchers: OK\n");
}


// ---- Test 3: TokenAtOffset variants --------------------------------
static void test_token_at_offset_variants(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    // "(a b)": tokens at byte ranges {0,1} {1,1} {2,1} {3,1} {4,1}.
    // Offset 0: SINGLE (start of LPAREN — also is offset 0 of root).
    //   Actually rowan considers offset 0 a boundary too? Let me check.
    //   At offset 0: LPAREN's range is [0,1]. Inclusive on both sides:
    //   0 is "at the start of LPAREN" — but there's no token before it,
    //   so just SINGLE.
    TokenAtOffset r0 = syntax_token_at_offset(root, 0);
    if (r0.kind != TOKEN_AT_OFFSET_SINGLE) DIE("offset 0: expected SINGLE");
    if (syntax_token_kind(r0.single) != SK_LPAREN) DIE("offset 0: wrong token");
    TOKEN_AT_OFFSET_RELEASE(r0);

    // Offset 1: boundary between LPAREN and 'a'. BETWEEN(LPAREN, 'a').
    TokenAtOffset r1 = syntax_token_at_offset(root, 1);
    if (r1.kind != TOKEN_AT_OFFSET_BETWEEN) DIE("offset 1: expected BETWEEN");
    if (syntax_token_kind(r1.left) != SK_LPAREN) DIE("offset 1 left wrong");
    if (strcmp(syntax_token_text(r1.right), "a") != 0) DIE("offset 1 right wrong");
    TOKEN_AT_OFFSET_RELEASE(r1);

    // Offset 5: end of the tree. There's no token starting at 5, so
    // SINGLE on the LAST token (RPAREN whose range is [4,1] → end == 5
    // is the inclusive end of RPAREN, and there's no right side).
    TokenAtOffset r5 = syntax_token_at_offset(root, 5);
    if (r5.kind != TOKEN_AT_OFFSET_SINGLE) DIE("offset 5: expected SINGLE");
    if (syntax_token_kind(r5.single) != SK_RPAREN) DIE("offset 5: wrong token");
    TOKEN_AT_OFFSET_RELEASE(r5);

    // Past end → NONE.
    TokenAtOffset r6 = syntax_token_at_offset(root, 6);
    if (r6.kind != TOKEN_AT_OFFSET_NONE) DIE("offset 6: expected NONE");

    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_token_at_offset_variants: OK\n");
}


// ---- Test 4: covering_element --------------------------------------
static void test_covering_element(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    // Range {3, 1} = 'b' token. Covering element is that token.
    SyntaxElement r1 = syntax_node_covering_element(root, (TextRange){.start = 3, .length = 1});
    if (r1.kind != SYNTAX_ELEM_TOKEN) DIE("range {3,1} cover should be a token");
    if (strcmp(syntax_token_text(r1.token), "b") != 0) DIE("range {3,1} not 'b'");
    SYN_ELEM_RELEASE(r1);

    // Range {1, 3} spans 'a', ' ', 'b' — no single child fully contains
    // it, so covering is LIST.
    SyntaxElement r2 = syntax_node_covering_element(root, (TextRange){.start = 1, .length = 3});
    if (r2.kind != SYNTAX_ELEM_NODE) DIE("range {1,3} cover should be a node");
    if (syntax_node_kind(r2.node) != SK_LIST) DIE("range {1,3} cover should be LIST");
    SYN_ELEM_RELEASE(r2);

    // Range {0, 5} = the whole LIST.
    SyntaxElement r3 = syntax_node_covering_element(root, (TextRange){.start = 0, .length = 5});
    if (r3.kind != SYNTAX_ELEM_NODE) DIE("range {0,5} cover should be a node");
    if (syntax_node_kind(r3.node) != SK_LIST) DIE("range {0,5} cover should be LIST");
    SYN_ELEM_RELEASE(r3);

    // Out-of-range → NONE.
    SyntaxElement r4 = syntax_node_covering_element(root, (TextRange){.start = 10, .length = 2});
    if (!syntax_element_is_none(r4)) DIE("out-of-range should be NONE");

    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_covering_element: OK\n");
}


// ---- Test 5: child_or_token_at_range -------------------------------
static void test_child_or_token_at_range(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);
    SyntaxNode *list = syntax_node_first_child(root);

    // Range {3,1} = 'b' — direct child of LIST.
    SyntaxElement e = syntax_node_child_or_token_at_range(list, (TextRange){.start = 3, .length = 1});
    if (e.kind != SYNTAX_ELEM_TOKEN) DIE("expected token");
    if (strcmp(syntax_token_text(e.token), "b") != 0) DIE("wrong token");
    SYN_ELEM_RELEASE(e);

    // Range {1,3} spans 3 children — no single child fully contains it.
    SyntaxElement n = syntax_node_child_or_token_at_range(list, (TextRange){.start = 1, .length = 3});
    if (!syntax_element_is_none(n)) DIE("expected NONE for spanning range");

    SYN_RELEASE(list);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_child_or_token_at_range: OK\n");
}


// ---- Test 6: siblings + siblings_with_tokens cursors ---------------
static void test_siblings(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);
    SyntaxNode *list = syntax_node_first_child(root);

    // siblings_with_tokens forward from index 2 (' ' WS): expects WS, 'b', ')'
    SyntaxElement ws_e = syntax_node_child_or_token(list, 2);
    SyntaxSiblingsElem it;
    syntax_siblings_elem_init_token(&it, ws_e.token, SYNTAX_DIR_NEXT);

    const SyntaxKind expected_kinds[] = {SK_WS, SK_WORD, SK_RPAREN};
    int count = 0;
    for (SyntaxElement e; ; ) {
        e = syntax_siblings_elem_next(&it);
        if (syntax_element_is_none(e)) break;
        if (count >= 3) DIE("too many siblings forward");
        SyntaxKind k = (e.kind == SYNTAX_ELEM_NODE) ? syntax_node_kind(e.node)
                                                    : syntax_token_kind(e.token);
        if (k != expected_kinds[count]) DIE("forward sib %d kind", count);
        SYN_ELEM_RELEASE(e);
        count++;
    }
    if (count != 3) DIE("expected 3 forward siblings, got %d", count);
    syntax_siblings_elem_free(&it);

    // Backward from index 2: WS, 'a', '('
    const SyntaxKind expected_back[] = {SK_WS, SK_WORD, SK_LPAREN};
    syntax_siblings_elem_init_token(&it, ws_e.token, SYNTAX_DIR_PREV);
    count = 0;
    for (SyntaxElement e; ; ) {
        e = syntax_siblings_elem_next(&it);
        if (syntax_element_is_none(e)) break;
        if (count >= 3) DIE("too many siblings backward");
        SyntaxKind k = (e.kind == SYNTAX_ELEM_NODE) ? syntax_node_kind(e.node)
                                                    : syntax_token_kind(e.token);
        if (k != expected_back[count]) DIE("backward sib %d kind", count);
        SYN_ELEM_RELEASE(e);
        count++;
    }
    if (count != 3) DIE("expected 3 backward siblings, got %d", count);
    syntax_siblings_elem_free(&it);

    SYN_ELEM_RELEASE(ws_e);
    SYN_RELEASE(list);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_siblings: OK\n");
}


// ---- Test 7: descendants_with_tokens -------------------------------
static void test_descendants_with_tokens(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    // Expected ENTER sequence under root (excluding root):
    //   LIST, '(', 'a', ' ', 'b', ')' — 6 elements.
    SyntaxDescendantsElem it;
    syntax_descendants_elem_init(&it, root);
    int count = 0;
    const SyntaxKind expected[] = {SK_LIST, SK_LPAREN, SK_WORD, SK_WS, SK_WORD, SK_RPAREN};
    for (SyntaxElement e; ; ) {
        e = syntax_descendants_elem_next(&it);
        if (syntax_element_is_none(e)) break;
        SyntaxKind k = (e.kind == SYNTAX_ELEM_NODE) ? syntax_node_kind(e.node)
                                                    : syntax_token_kind(e.token);
        if (count >= 6) DIE("too many descendants");
        if (k != expected[count]) DIE("descendant %d kind = %u, want %u",
                                       count, k, expected[count]);
        SYN_ELEM_RELEASE(e);
        count++;
    }
    if (count != 6) DIE("expected 6 descendants, got %d", count);
    syntax_descendants_elem_free(&it);

    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_descendants_with_tokens: OK\n");
}


// ---- Test 8: next_token / prev_token cross-parent walk -------------
static void test_next_prev_token(void) {
    NodeCache *cache = node_cache_new();
    // Build a nested tree:  ROOT( LIST( '(' WORD('a') LIST( '(' WORD('b') ')' ) ')' ) )
    GreenBuilder *b = green_builder_new(cache);
    green_builder_start_node(b, SK_ROOT);
        green_builder_start_node(b, SK_LIST);
            green_builder_token(b, SK_LPAREN, "(", 1);
            green_builder_token(b, SK_WORD,   "a", 1);
            green_builder_start_node(b, SK_LIST);
                green_builder_token(b, SK_LPAREN, "(", 1);
                green_builder_token(b, SK_WORD,   "b", 1);
                green_builder_token(b, SK_RPAREN, ")", 1);
            green_builder_finish_node(b);
            green_builder_token(b, SK_RPAREN, ")", 1);
        green_builder_finish_node(b);
    green_builder_finish_node(b);
    GreenNode *g = green_builder_finish(b);
    green_builder_destroy(b);

    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    // Walk all tokens forward starting from the first.
    SyntaxToken *t = syntax_node_first_token(root);
    const char *expected_fwd[] = {"(", "a", "(", "b", ")", ")"};
    int count = 0;
    while (t) {
        if (count >= 6) DIE("too many tokens forward");
        if (strcmp(syntax_token_text(t), expected_fwd[count]) != 0)
            DIE("forward token %d = '%s', want '%s'",
                count, syntax_token_text(t), expected_fwd[count]);
        SyntaxToken *next = syntax_token_next_token(t);
        SYN_RELEASE(t);
        t = next;
        count++;
    }
    if (count != 6) DIE("expected 6 tokens forward, got %d", count);

    // Walk backward from the last.
    t = syntax_node_last_token(root);
    const char *expected_back[] = {")", ")", "b", "(", "a", "("};
    count = 0;
    while (t) {
        if (count >= 6) DIE("too many tokens backward");
        if (strcmp(syntax_token_text(t), expected_back[count]) != 0)
            DIE("backward token %d = '%s', want '%s'",
                count, syntax_token_text(t), expected_back[count]);
        SyntaxToken *prev = syntax_token_prev_token(t);
        SYN_RELEASE(t);
        t = prev;
        count++;
    }
    if (count != 6) DIE("expected 6 tokens backward, got %d", count);

    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_next_prev_token: OK\n");
}


// ---- Test 9: token.ancestors ---------------------------------------
static void test_token_ancestors(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    SyntaxToken *first = syntax_node_first_token(root);  // '('
    SyntaxAncestors it;
    syntax_token_ancestors_init(&it, first);

    // Expected: LIST, ROOT.
    const SyntaxKind expected[] = {SK_LIST, SK_ROOT};
    int count = 0;
    for (SyntaxNode *n; (n = syntax_ancestors_next(&it)); ) {
        if (count >= 2) DIE("too many ancestors");
        if (syntax_node_kind(n) != expected[count]) DIE("ancestor %d kind", count);
        SYN_RELEASE(n);
        count++;
    }
    if (count != 2) DIE("expected 2 ancestors, got %d", count);
    syntax_ancestors_free(&it);

    SYN_RELEASE(first);
    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_token_ancestors: OK\n");
}


// ---- Test 10: char_at / find_char / contains_char ------------------
//
// ASCII-only first; then a multi-byte UTF-8 case to verify decoding.
static void test_char_api_ascii(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);  // "(a b)"
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);
    SyntaxText st = syntax_text_of(root);

    // ASCII source: char_at == byte_at for every offset.
    for (uint32_t i = 0; i < 5; i++) {
        int32_t c = syntax_text_char_at(&st, i);
        int b = syntax_text_byte_at(&st, i);
        if (c != b) DIE("char_at(%u)=%d != byte_at=%d", i, c, b);
    }

    // find_char for ASCII = find_byte.
    if (syntax_text_find_char(&st, 'a') != 1) DIE("find_char 'a'");
    if (syntax_text_find_char(&st, 'z') != UINT32_MAX) DIE("find_char 'z'");
    if (!syntax_text_contains_char(&st, ')')) DIE("contains ')'");
    if (syntax_text_contains_char(&st, 'Z')) DIE("contains 'Z'?");

    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_char_api_ascii: OK\n");
}

// Multi-byte UTF-8: build a tree with the string "café" (5 bytes: c, a, f, 0xC3, 0xA9).
static void test_char_api_utf8(void) {
    NodeCache *cache = node_cache_new();
    GreenBuilder *b = green_builder_new(cache);
    green_builder_start_node(b, SK_ROOT);
        green_builder_token(b, SK_WORD, "caf\xc3\xa9", 5);  // "café"
    green_builder_finish_node(b);
    GreenNode *g = green_builder_finish(b);
    green_builder_destroy(b);

    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);
    SyntaxText st = syntax_text_of(root);

    // Offsets 0,1,2 are ASCII; 3 is the start of the 'é' 2-byte sequence (0xC3, 0xA9).
    if (syntax_text_char_at(&st, 0) != 'c') DIE("char_at(0)");
    if (syntax_text_char_at(&st, 1) != 'a') DIE("char_at(1)");
    if (syntax_text_char_at(&st, 2) != 'f') DIE("char_at(2)");
    // 'é' code point is U+00E9 = 233.
    int32_t e = syntax_text_char_at(&st, 3);
    if (e != 0x00E9) DIE("char_at(3) = %d, want 233 (é)", e);
    // Offset 4 is inside the multi-byte sequence — should be -1.
    if (syntax_text_char_at(&st, 4) != -1) DIE("char_at(4) should be -1 (mid-sequence)");

    // find_char on 'é' should locate it at offset 3.
    if (syntax_text_find_char(&st, 0x00E9) != 3) DIE("find_char 'é'");
    if (!syntax_text_contains_char(&st, 0x00E9)) DIE("contains 'é'");

    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_char_api_utf8: OK\n");
}


// ---- Test 11: try_fold_chunks --------------------------------------
typedef struct {
    int chunks_visited;
} FoldCount;

static bool count_then_abort(const char *text, uint32_t len, void *acc, void *user) {
    (void)text; (void)len; (void)user;
    FoldCount *c = (FoldCount *)acc;
    c->chunks_visited++;
    return c->chunks_visited < 2;  // abort after 2 chunks
}

static void test_try_fold_chunks(void) {
    NodeCache *cache = node_cache_new();
    GreenNode *g = build_paren(cache);
    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);
    SyntaxText st = syntax_text_of(root);

    FoldCount c = {.chunks_visited = 0};
    bool completed = syntax_text_try_fold_chunks(&st, count_then_abort, &c, NULL);
    if (completed) DIE("fold should have aborted");
    if (c.chunks_visited != 2) DIE("chunks_visited = %d, want 2", c.chunks_visited);

    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_try_fold_chunks: OK\n");
}


// ---- Test 12: preorder skip_subtree --------------------------------
static void test_preorder_skip_subtree(void) {
    NodeCache *cache = node_cache_new();
    // Build:  ROOT( LIST( '(' 'a' ')' ) )
    GreenBuilder *b = green_builder_new(cache);
    green_builder_start_node(b, SK_ROOT);
        green_builder_start_node(b, SK_LIST);
            green_builder_token(b, SK_LPAREN, "(", 1);
            green_builder_token(b, SK_WORD,   "a", 1);
            green_builder_token(b, SK_RPAREN, ")", 1);
        green_builder_finish_node(b);
    green_builder_finish_node(b);
    GreenNode *g = green_builder_finish(b);
    green_builder_destroy(b);

    SyntaxTree *tree = syntax_tree_new(g);
    SyntaxNode *root = syntax_tree_root(tree);

    SyntaxPreorder po;
    syntax_preorder_init(&po, root);

    // Sequence WITHOUT skip would yield: Enter(ROOT), Enter(LIST),
    // Enter('('), Leave('('), Enter('a'), Leave('a'), Enter(')'),
    // Leave(')'), Leave(LIST), Leave(ROOT).
    //
    // With skip_subtree called after Enter(LIST):
    //   Enter(ROOT), Enter(LIST), [skip!], Leave(LIST), Leave(ROOT).
    int events = 0;
    SyntaxKind seen[8] = {0};
    SyntaxWalkEventKind seen_kind[8] = {0};
    for (;;) {
        SyntaxWalkEvent ev = syntax_preorder_next(&po);
        if (syntax_walk_event_is_none(ev)) break;
        if (events >= 8) DIE("too many events");
        SyntaxKind k = (ev.element.kind == SYNTAX_ELEM_NODE)
                           ? syntax_node_kind(ev.element.node)
                           : syntax_token_kind(ev.element.token);
        seen[events] = k;
        seen_kind[events] = ev.kind;
        // After we see Enter(LIST), call skip_subtree.
        if (ev.kind == SYNTAX_WALK_ENTER && k == SK_LIST) {
            syntax_preorder_skip_subtree(&po);
        }
        SYN_ELEM_RELEASE(ev.element);
        events++;
    }
    syntax_preorder_free(&po);

    // Expected: Enter ROOT, Enter LIST, Leave LIST, Leave ROOT = 4 events.
    if (events != 4) DIE("events = %d, want 4", events);
    if (seen_kind[0] != SYNTAX_WALK_ENTER || seen[0] != SK_ROOT) DIE("ev0");
    if (seen_kind[1] != SYNTAX_WALK_ENTER || seen[1] != SK_LIST) DIE("ev1");
    if (seen_kind[2] != SYNTAX_WALK_LEAVE || seen[2] != SK_LIST) DIE("ev2");
    if (seen_kind[3] != SYNTAX_WALK_LEAVE || seen[3] != SK_ROOT) DIE("ev3");

    SYN_RELEASE(root);
    syntax_tree_free(tree);
    green_node_release(g);
    node_cache_destroy(cache);
    fprintf(stderr, "  test_preorder_skip_subtree: OK\n");
}


int main(void) {
    fprintf(stderr, "syntax_extras_test: starting\n");
    test_clone_subtree();
    test_by_kind_matchers();
    test_token_at_offset_variants();
    test_covering_element();
    test_child_or_token_at_range();
    test_siblings();
    test_descendants_with_tokens();
    test_next_prev_token();
    test_token_ancestors();
    test_char_api_ascii();
    test_char_api_utf8();
    test_try_fold_chunks();
    test_preorder_skip_subtree();
    fprintf(stderr, "syntax_extras_test: all PASS\n");
    return 0;
}
