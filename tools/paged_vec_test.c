// PagedVec<T> unit tests.
//
// Verifies the load-bearing property: pointer stability across pushes.
// A pointer obtained from paged_get before the structure grows must
// remain valid and dereferenceable after subsequent pushes. This is
// the entire point of paged_vec; if it fails, the whole engine design
// premise (const T * returns from result_columns.h) collapses.
//
// Build: standalone, links only paged_vec.c (+ stdlib). ASan-enabled
// via TEST_CFLAGS in the Makefile rule.

#include "../src/support/data_structure/paged_vec.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIE(...) do { fprintf(stderr, "paged_vec_test: " __VA_ARGS__); \
                       fprintf(stderr, "\n"); exit(1); } while (0)

#define CHECK(cond, ...) do { if (!(cond)) DIE(__VA_ARGS__); } while (0)


// ---------------------------------------------------------------------
// Test 1: basic init + push + get for a single element.
// ---------------------------------------------------------------------
static void test_basic(void) {
    PagedVec p;
    paged_init(&p, sizeof(uint32_t));
    CHECK(paged_count(&p) == 0, "fresh PagedVec count != 0");
    CHECK(paged_get(&p, 0) == NULL, "OOB get must return NULL");

    uint32_t x = 42;
    size_t idx = paged_push(&p, &x);
    CHECK(idx == 0, "first push must return index 0");
    CHECK(paged_count(&p) == 1, "count after one push != 1");

    uint32_t *got = paged_get(&p, 0);
    CHECK(got != NULL, "in-bounds get returned NULL");
    CHECK(*got == 42, "round-trip value mismatch");

    paged_free(&p);
    printf("test_basic: ok\n");
}


// ---------------------------------------------------------------------
// Test 2: page boundary crossing. PAGED_PAGE_LEN = 1024; pushing 1025
// elements forces a second page allocation.
// ---------------------------------------------------------------------
static void test_page_boundary(void) {
    PagedVec p;
    paged_init(&p, sizeof(uint32_t));

    // Fill exactly one page.
    for (uint32_t i = 0; i < PAGED_PAGE_LEN; i++) {
        size_t idx = paged_push(&p, &i);
        CHECK(idx == i, "push #%u returned wrong index %zu", i, idx);
    }
    CHECK(paged_count(&p) == PAGED_PAGE_LEN, "count after one page wrong");
    CHECK(p.page_count == 1, "expected 1 page allocated");

    // Push one more — forces second page.
    uint32_t x = 99999;
    size_t idx = paged_push(&p, &x);
    CHECK(idx == PAGED_PAGE_LEN, "page-boundary push returned wrong index");
    CHECK(p.page_count == 2, "expected 2 pages after boundary crossing");

    // Read back all elements.
    for (uint32_t i = 0; i < PAGED_PAGE_LEN; i++) {
        uint32_t *got = paged_get(&p, i);
        CHECK(*got == i, "round-trip mismatch at index %u", i);
    }
    uint32_t *last = paged_get(&p, PAGED_PAGE_LEN);
    CHECK(*last == 99999, "round-trip mismatch at page-boundary index");

    paged_free(&p);
    printf("test_page_boundary: ok\n");
}


// ---------------------------------------------------------------------
// Test 3: POINTER STABILITY. THE LOAD-BEARING TEST.
// Hold a pointer to element 5, push 100,000 more elements (~98 pages),
// dereference the original pointer. Must still be valid + unchanged.
// ---------------------------------------------------------------------
static void test_pointer_stability(void) {
    PagedVec p;
    paged_init(&p, sizeof(uint32_t));

    // Push first 10 elements.
    for (uint32_t i = 0; i < 10; i++) {
        paged_push(&p, &i);
    }

    // Capture a pointer into the FIRST page.
    uint32_t *captured = paged_get(&p, 5);
    CHECK(*captured == 5, "captured pointer initial value wrong");

    // Now push 100,000 more elements, crossing many page boundaries.
    for (uint32_t i = 10; i < 100010; i++) {
        paged_push(&p, &i);
    }
    CHECK(paged_count(&p) == 100010, "expected 100010 elements");
    CHECK(p.page_count >= 97, "expected many pages allocated");

    // The captured pointer must still resolve to the same value.
    // If PagedVec invalidated pointers (Vec-style realloc), this would
    // be a use-after-free; ASan would catch it.
    CHECK(*captured == 5,
          "POINTER STABILITY FAILED: captured value changed after pushes "
          "(saw %u, expected 5)", *captured);

    // Cross-check via paged_get.
    uint32_t *via_get = paged_get(&p, 5);
    CHECK(via_get == captured,
          "paged_get returned a different pointer (%p vs %p) for the same index",
          (void *)via_get, (void *)captured);

    paged_free(&p);
    printf("test_pointer_stability: ok\n");
}


// ---------------------------------------------------------------------
// Test 4: paged_clear resets count; subsequent pushes overwrite in-place.
// ---------------------------------------------------------------------
static void test_clear(void) {
    PagedVec p;
    paged_init(&p, sizeof(uint32_t));

    for (uint32_t i = 0; i < 100; i++) paged_push(&p, &i);
    CHECK(paged_count(&p) == 100, "expected 100 elements before clear");
    size_t pages_before = p.page_count;

    paged_clear(&p);
    CHECK(paged_count(&p) == 0, "count after clear != 0");
    CHECK(p.page_count == pages_before,
          "paged_clear must keep pages allocated for reuse");

    // Push 50 new elements; they should land in the same pages.
    for (uint32_t i = 0; i < 50; i++) {
        uint32_t v = 1000 + i;
        paged_push(&p, &v);
    }
    CHECK(paged_count(&p) == 50, "count after re-push wrong");
    CHECK(p.page_count == pages_before, "no new page should have been needed");

    uint32_t *got = paged_get(&p, 0);
    CHECK(*got == 1000, "first element after re-push wrong (%u)", *got);

    paged_free(&p);
    printf("test_clear: ok\n");
}


// ---------------------------------------------------------------------
// Test 5: paged_push_zero gives zero-initialized slots.
// ---------------------------------------------------------------------
static void test_push_zero(void) {
    typedef struct { uint64_t a; uint64_t b; uint32_t c; uint8_t d[20]; } Big;
    PagedVec p;
    paged_init(&p, sizeof(Big));

    for (int i = 0; i < 1500; i++) {
        size_t idx = paged_push_zero(&p);
        Big *b = paged_get(&p, idx);
        CHECK(b->a == 0 && b->b == 0 && b->c == 0 && b->d[0] == 0 && b->d[19] == 0,
              "paged_push_zero element #%d wasn't zero", i);
    }

    paged_free(&p);
    printf("test_push_zero: ok\n");
}


// ---------------------------------------------------------------------
// Test 6: free at various sizes (0, 1, 1023, 1024, 1025, 1M).
// Run under ASan/LSan to catch leaks.
// ---------------------------------------------------------------------
static void test_free_sizes(void) {
    const size_t sizes[] = {0, 1, 1023, 1024, 1025, 100000, 1000000};
    for (size_t s = 0; s < sizeof(sizes)/sizeof(*sizes); s++) {
        PagedVec p;
        paged_init(&p, sizeof(uint64_t));
        for (uint64_t i = 0; i < sizes[s]; i++) {
            paged_push(&p, &i);
        }
        paged_free(&p);
    }
    printf("test_free_sizes: ok\n");
}


// ---------------------------------------------------------------------
// Test 7: large element type (cacheline-sized struct).
// Verifies element_size is honored across the whole index range.
// ---------------------------------------------------------------------
static void test_large_element(void) {
    typedef struct { uint64_t data[8]; } Cacheline;  // 64 bytes
    PagedVec p;
    paged_init(&p, sizeof(Cacheline));

    for (uint64_t i = 0; i < 5000; i++) {
        Cacheline c;
        for (int j = 0; j < 8; j++) c.data[j] = i * 10 + j;
        size_t idx = paged_push(&p, &c);
        CHECK(idx == i, "wrong index at large-element push %llu",
              (unsigned long long)i);
    }
    for (uint64_t i = 0; i < 5000; i++) {
        Cacheline *got = paged_get(&p, i);
        for (int j = 0; j < 8; j++) {
            CHECK(got->data[j] == i * 10 + j,
                  "cacheline mismatch at i=%llu j=%d", (unsigned long long)i, j);
        }
    }

    paged_free(&p);
    printf("test_large_element: ok\n");
}


// ---------------------------------------------------------------------
int main(void) {
    test_basic();
    test_page_boundary();
    test_pointer_stability();
    test_clear();
    test_push_zero();
    test_free_sizes();
    test_large_element();
    printf("paged_vec_test: all 7 tests passed\n");
    return 0;
}
