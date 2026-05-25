// SLL primitive tests. Exercises sll_link/add/unlink/adjust against a
// TestElem that has zero NodeData/SyntaxNode coupling — proves the SLL
// is standalone and reusable.

#include "../src/syntax/sll.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIE(...) do { fprintf(stderr, "syntax_sll_test: " __VA_ARGS__); \
                       fprintf(stderr, "\n"); exit(1); } while (0)


typedef struct TestElem {
    SllNode sll;
    int     payload;
} TestElem;

static TestElem *te_new(uint32_t key, int payload) {
    TestElem *e = (TestElem *)calloc(1, sizeof(TestElem));
    if (!e) DIE("oom");
    e->sll.key = key;
    e->payload = payload;
    return e;
}


// Walk the ring starting at head; verify bi-directional links + sorted-
// ascending order (with the head being the minimum). Returns element
// count; aborts on inconsistency.
static uint32_t verify_ring(SllNode *head) {
    if (head == NULL) return 0;
    SllNode *curr = head;
    uint32_t count = 0;
    uint32_t last_key = head->key;
    bool first = true;
    do {
        // Bi-directional consistency.
        if (curr->next->prev != curr)
            DIE("ring inconsistency: curr->next->prev != curr (count=%u)", count);
        if (curr->prev->next != curr)
            DIE("ring inconsistency: curr->prev->next != curr (count=%u)", count);
        // Sorted: each curr->key >= last_key, except the wrap-around at
        // head where we compare curr->key (largest in ring) >= head->key.
        if (!first && curr->key < last_key)
            DIE("ring not sorted: curr->key=%u < last_key=%u", curr->key, last_key);
        last_key = curr->key;
        first = false;
        count++;
        curr = curr->next;
        if (count > 10000) DIE("ring walk did not terminate");
    } while (curr != head);
    return count;
}


// ---- Test 1: empty head, single insert -----------------------------
static void test_empty_insert(void) {
    SllNode *head = NULL;
    TestElem *e = te_new(5, 100);
    SllAddResult r = sll_init(&head, &e->sll);
    if (r.kind != SLL_ADD_EMPTY_HEAD) DIE("expected EMPTY_HEAD, got %d", r.kind);
    if (head != &e->sll) DIE("head not set");
    if (e->sll.prev != &e->sll || e->sll.next != &e->sll) DIE("not self-linked");
    if (verify_ring(head) != 1) DIE("ring size != 1");

    sll_unlink(&head, &e->sll);
    if (head != NULL) DIE("head not NULL after unlink");
    free(e);
    fprintf(stderr, "  test_empty_insert: OK\n");
}


// ---- Test 2: smaller than head → replaces head ---------------------
static void test_smaller_than_head(void) {
    SllNode *head = NULL;
    TestElem *a = te_new(10, 0), *b = te_new(5, 0);
    sll_init(&head, &a->sll);
    SllAddResult r = sll_init(&head, &b->sll);
    if (r.kind != SLL_ADD_SMALLER_THAN_HEAD) DIE("expected SMALLER_THAN_HEAD, got %d", r.kind);
    if (head != &b->sll) DIE("head not b");
    if (verify_ring(head) != 2) DIE("ring size != 2");
    if (head->next != &a->sll) DIE("b->next != a");
    if (head->prev != &a->sll) DIE("b->prev != a");

    sll_unlink(&head, &a->sll);
    sll_unlink(&head, &b->sll);
    free(a); free(b);
    fprintf(stderr, "  test_smaller_than_head: OK\n");
}


// ---- Test 3: insert in middle preserves ordering -------------------
static void test_insert_middle(void) {
    SllNode *head = NULL;
    TestElem *a = te_new(2, 0), *b = te_new(8, 0), *c = te_new(5, 0);
    sll_init(&head, &a->sll);
    sll_init(&head, &b->sll);
    SllAddResult r = sll_init(&head, &c->sll);
    if (r.kind != SLL_ADD_SMALLER_THAN_NOT_HEAD) DIE("expected SMALLER_THAN_NOT_HEAD, got %d", r.kind);
    if (verify_ring(head) != 3) DIE("ring size != 3");
    // Order: a(2) → c(5) → b(8) → a
    if (head != &a->sll) DIE("head not a");
    if (head->next != &c->sll) DIE("a->next != c");
    if (head->next->next != &b->sll) DIE("c->next != b");
    if (head->next->next->next != &a->sll) DIE("b->next != a (wrap)");

    sll_unlink(&head, &c->sll);
    sll_unlink(&head, &b->sll);
    sll_unlink(&head, &a->sll);
    free(a); free(b); free(c);
    fprintf(stderr, "  test_insert_middle: OK\n");
}


// ---- Test 4: duplicate key returns ALREADY_IN_SLL ------------------
static void test_duplicate_key(void) {
    SllNode *head = NULL;
    TestElem *a = te_new(5, 0), *b = te_new(5, 0);
    sll_init(&head, &a->sll);
    SllAddResult r = sll_link(&head, &b->sll);
    if (r.kind != SLL_ADD_ALREADY_IN_SLL) DIE("expected ALREADY_IN_SLL, got %d", r.kind);
    if (r.curr != &a->sll) DIE("ALREADY_IN_SLL.curr != a");
    // Don't call sll_add — that's the "panic in rowan" path.
    if (verify_ring(head) != 1) DIE("ring should still have 1 elem");

    sll_unlink(&head, &a->sll);
    free(a); free(b);
    fprintf(stderr, "  test_duplicate_key: OK\n");
}


// ---- Test 5: unlink head with successor → head moves ---------------
static void test_unlink_head_with_successor(void) {
    SllNode *head = NULL;
    TestElem *a = te_new(2, 0), *b = te_new(5, 0), *c = te_new(8, 0);
    sll_init(&head, &a->sll); sll_init(&head, &b->sll); sll_init(&head, &c->sll);
    sll_unlink(&head, &a->sll);
    if (head != &b->sll) DIE("head should move to b");
    if (verify_ring(head) != 2) DIE("ring size != 2");

    sll_unlink(&head, &b->sll); sll_unlink(&head, &c->sll);
    free(a); free(b); free(c);
    fprintf(stderr, "  test_unlink_head_with_successor: OK\n");
}


// ---- Test 6: unlink last element → head NULL -----------------------
static void test_unlink_last(void) {
    SllNode *head = NULL;
    TestElem *a = te_new(5, 0);
    sll_init(&head, &a->sll);
    sll_unlink(&head, &a->sll);
    if (head != NULL) DIE("head should be NULL");
    // Verify defensive self-link.
    if (a->sll.prev != &a->sll || a->sll.next != &a->sll) DIE("not self-linked after unlink");
    free(a);
    fprintf(stderr, "  test_unlink_last: OK\n");
}


// ---- Test 7: unlink interior --------------------------------------
static void test_unlink_interior(void) {
    SllNode *head = NULL;
    TestElem *a = te_new(2, 0), *b = te_new(5, 0), *c = te_new(8, 0);
    sll_init(&head, &a->sll); sll_init(&head, &b->sll); sll_init(&head, &c->sll);
    sll_unlink(&head, &b->sll);
    if (head != &a->sll) DIE("head should still be a");
    if (verify_ring(head) != 2) DIE("ring size != 2");
    if (head->next != &c->sll) DIE("a->next should be c");
    if (head->prev != &c->sll) DIE("a->prev should be c");

    sll_unlink(&head, &a->sll); sll_unlink(&head, &c->sll);
    free(a); free(b); free(c);
    fprintf(stderr, "  test_unlink_interior: OK\n");
}


// ---- Test 8: adjust +1 from key 5 ---------------------------------
static void test_adjust_positive(void) {
    SllNode *head = NULL;
    TestElem *a = te_new(2, 0), *b = te_new(5, 0), *c = te_new(8, 0);
    sll_init(&head, &a->sll); sll_init(&head, &b->sll); sll_init(&head, &c->sll);

    sll_adjust(head, /*from=*/5, /*delta=*/+1);
    if (a->sll.key != 2) DIE("a->key should remain 2");
    if (b->sll.key != 6) DIE("b->key should become 6");
    if (c->sll.key != 9) DIE("c->key should become 9");

    sll_unlink(&head, &a->sll); sll_unlink(&head, &b->sll); sll_unlink(&head, &c->sll);
    free(a); free(b); free(c);
    fprintf(stderr, "  test_adjust_positive: OK\n");
}


// ---- Test 9: adjust -1 from key 5 ---------------------------------
static void test_adjust_negative(void) {
    SllNode *head = NULL;
    TestElem *a = te_new(2, 0), *b = te_new(5, 0), *c = te_new(8, 0);
    sll_init(&head, &a->sll); sll_init(&head, &b->sll); sll_init(&head, &c->sll);

    sll_adjust(head, /*from=*/5, /*delta=*/-1);
    if (a->sll.key != 2) DIE("a->key should remain 2");
    if (b->sll.key != 4) DIE("b->key should become 4");
    if (c->sll.key != 7) DIE("c->key should become 7");

    sll_unlink(&head, &a->sll); sll_unlink(&head, &b->sll); sll_unlink(&head, &c->sll);
    free(a); free(b); free(c);
    fprintf(stderr, "  test_adjust_negative: OK\n");
}


// ---- Test 10: stress -----------------------------------------------
//
// Build a ring of N elements with random keys, randomly unlink half of
// them, then verify ring integrity + remaining count. Repeat many times.
// Each iteration starts fresh; cumulative coverage exercises rare
// pointer states (1-element, 2-element, unlink-of-head + new head).
#define STRESS_N      32
#define STRESS_ROUNDS 200

static void test_stress(void) {
    srand(0x5717);

    for (uint32_t round = 0; round < STRESS_ROUNDS; round++) {
        SllNode *head = NULL;
        TestElem *elems[STRESS_N];
        uint8_t  alive[STRESS_N];
        uint32_t alive_count = 0;

        for (uint32_t i = 0; i < STRESS_N; i++) {
            elems[i] = te_new(i, (int)i);   // unique keys 0..N-1
            // Randomize insert order by sometimes skipping
            if ((rand() & 1) == 0) {
                sll_init(&head, &elems[i]->sll);
                alive[i] = 1;
                alive_count++;
            } else {
                alive[i] = 0;
            }
        }

        uint32_t observed = verify_ring(head);
        if (observed != alive_count) DIE("round %u: inserted %u but ring has %u",
                                          round, alive_count, observed);

        // Now insert the rest (in a different order).
        for (uint32_t i = 0; i < STRESS_N; i++) {
            if (!alive[i]) {
                sll_init(&head, &elems[i]->sll);
                alive[i] = 1;
                alive_count++;
            }
        }
        if (verify_ring(head) != STRESS_N) DIE("round %u: full ring size mismatch", round);

        // Randomly unlink half.
        for (uint32_t i = 0; i < STRESS_N / 2; i++) {
            uint32_t pick;
            do { pick = (uint32_t)(rand() % STRESS_N); } while (!alive[pick]);
            sll_unlink(&head, &elems[pick]->sll);
            alive[pick] = 0;
            alive_count--;
            // Sanity: ring still consistent
            if (verify_ring(head) != alive_count)
                DIE("round %u: after unlink %u, expected %u observed %u",
                    round, pick, alive_count, verify_ring(head));
        }

        // Unlink remaining.
        for (uint32_t i = 0; i < STRESS_N; i++) {
            if (alive[i]) {
                sll_unlink(&head, &elems[i]->sll);
                alive[i] = 0;
            }
        }
        if (head != NULL) DIE("round %u: head not NULL after full drain", round);

        for (uint32_t i = 0; i < STRESS_N; i++) free(elems[i]);
    }
    fprintf(stderr, "  test_stress: OK (%d rounds)\n", STRESS_ROUNDS);
}


int main(void) {
    fprintf(stderr, "syntax_sll_test: starting\n");
    test_empty_insert();
    test_smaller_than_head();
    test_insert_middle();
    test_duplicate_key();
    test_unlink_head_with_successor();
    test_unlink_last();
    test_unlink_interior();
    test_adjust_positive();
    test_adjust_negative();
    test_stress();
    fprintf(stderr, "syntax_sll_test: all PASS\n");
    return 0;
}
