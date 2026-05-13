// Unit tests for the malloc-default / arena-fixed Vec.
//
// Covers:
//   T1 — vec_init produces an empty malloc-backed Vec with capacity 0
//   T2 — vec_push allocates and grows via realloc; data survives
//   T3 — vec_push_zero appends zero-filled elements
//   T4 — vec_get returns NULL on out-of-range; valid pointer otherwise
//   T5 — vec_clear resets count without releasing buffer
//   T6 — vec_init_in_arena: fixed capacity slab in an arena
//   T7 — Arena-backed Vec under capacity accepts pushes
//   T8 — Pointer stability for malloc-backed across grow events:
//        Vec.data may move; vec_get always returns a valid pointer
//   T9 — Large workload (100k u32 pushes) — count + readback correct
//
// Run via `make test-vec`.

#include "../src/db/storage/vec.h"
#include "../src/db/storage/arena.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;
static const char *g_current = NULL;

static void start(const char *name) {
    g_current = name;
    printf("  ... %s\n", name);
}
static void finish(bool ok) {
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        fprintf(stderr, "       FAIL: %s\n", g_current);
    }
}

// =====================================================================

static void test_init_default(void) {
    start("vec_init: empty malloc-backed Vec");
    Vec v;
    vec_init(&v, sizeof(uint32_t));

    bool ok = (v.data == NULL);
    ok &= (v.count == 0);
    ok &= (v.capacity == 0);
    ok &= (v.element_size == sizeof(uint32_t));
    ok &= (v.arena == NULL);

    finish(ok);
    vec_free(&v);
}

static void test_push_and_get(void) {
    start("vec_push appends elements; vec_get reads them back");
    Vec v;
    vec_init(&v, sizeof(uint32_t));

    uint32_t a = 10, b = 20, c = 30;
    vec_push(&v, &a);
    vec_push(&v, &b);
    vec_push(&v, &c);

    bool ok = (v.count == 3);
    ok &= (v.capacity >= 3);
    ok &= (*(uint32_t *)vec_get(&v, 0) == 10);
    ok &= (*(uint32_t *)vec_get(&v, 1) == 20);
    ok &= (*(uint32_t *)vec_get(&v, 2) == 30);

    finish(ok);
    vec_free(&v);
}

static void test_push_zero(void) {
    start("vec_push_zero appends zero-filled elements");
    Vec v;
    vec_init(&v, sizeof(uint64_t));

    vec_push_zero(&v);
    vec_push_zero(&v);

    bool ok = (v.count == 2);
    ok &= (*(uint64_t *)vec_get(&v, 0) == 0);
    ok &= (*(uint64_t *)vec_get(&v, 1) == 0);

    finish(ok);
    vec_free(&v);
}

static void test_get_oob(void) {
    start("vec_get returns NULL on out-of-range; valid in range");
    Vec v;
    vec_init(&v, sizeof(uint32_t));
    uint32_t x = 42;
    vec_push(&v, &x);

    bool ok = (vec_get(&v, 0) != NULL);
    ok &= (vec_get(&v, 1) == NULL);
    ok &= (vec_get(&v, 999) == NULL);

    finish(ok);
    vec_free(&v);
}

static void test_clear(void) {
    start("vec_clear resets count without freeing");
    Vec v;
    vec_init(&v, sizeof(uint32_t));
    for (uint32_t i = 0; i < 10; i++) vec_push(&v, &i);

    size_t cap_before = v.capacity;
    void  *data_before = v.data;
    vec_clear(&v);

    bool ok = (v.count == 0);
    ok &= (v.capacity == cap_before);  // buffer retained
    ok &= (v.data == data_before);

    // Can push again into the retained buffer.
    uint32_t y = 99;
    vec_push(&v, &y);
    ok &= (*(uint32_t *)vec_get(&v, 0) == 99);

    finish(ok);
    vec_free(&v);
}

static void test_arena_fixed(void) {
    start("vec_init_in_arena: fixed-capacity arena slab");
    Arena arena;
    arena_init(&arena, 4096);

    Vec v;
    vec_init_in_arena(&v, &arena, /*max_count=*/8, sizeof(uint32_t));

    bool ok = (v.capacity == 8);
    ok &= (v.count == 0);
    ok &= (v.arena == &arena);
    ok &= (v.data != NULL);  // slab pre-allocated

    for (uint32_t i = 1; i <= 8; i++) vec_push(&v, &i);
    ok &= (v.count == 8);
    ok &= (*(uint32_t *)vec_get(&v, 0) == 1);
    ok &= (*(uint32_t *)vec_get(&v, 7) == 8);

    // vec_free on arena-backed is a no-op for the storage; we test
    // that it doesn't crash. The arena_free below reclaims memory.
    vec_free(&v);
    arena_free(&arena);
    finish(ok);
}

static void test_pointer_stability_via_get(void) {
    start("vec_get after grow returns valid pointers");
    Vec v;
    vec_init(&v, sizeof(uint64_t));

    // Push enough to force multiple realloc grows.
    for (uint64_t i = 0; i < 1000; i++) {
        vec_push(&v, &i);
    }

    // Re-read via vec_get — pointer is recomputed each call so it
    // resolves correctly even though the data buffer moved during grows.
    bool ok = (v.count == 1000);
    for (uint64_t i = 0; i < 1000; i++) {
        uint64_t got = *(uint64_t *)vec_get(&v, i);
        if (got != i) { ok = false; break; }
    }

    finish(ok);
    vec_free(&v);
}

static void test_large_workload(void) {
    start("100k u32 pushes: count + readback correct");
    Vec v;
    vec_init(&v, sizeof(uint32_t));

    const uint32_t N = 100000;
    for (uint32_t i = 0; i < N; i++) {
        uint32_t v_i = i * 3 + 7;
        vec_push(&v, &v_i);
    }

    bool ok = (v.count == N);
    for (uint32_t i = 0; i < N; i++) {
        uint32_t got = *(uint32_t *)vec_get(&v, i);
        if (got != i * 3 + 7) { ok = false; break; }
    }

    finish(ok);
    vec_free(&v);
}

// =====================================================================

int main(void) {
    printf("vec unit tests\n");

    test_init_default();
    test_push_and_get();
    test_push_zero();
    test_get_oob();
    test_clear();
    test_arena_fixed();
    test_pointer_stability_via_get();
    test_large_workload();

    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
