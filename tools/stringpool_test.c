// Unit tests for the content-deduped StringPool.
//
// Covers:
//   T1 — pool_init seeds the offset-0 sentinel; slot table is all-empty
//   T2 — pool_intern("") fast-paths to StrId{0} (no slot allocation)
//   T3 — pool_intern of identical strings returns the same StrId
//   T4 — pool_intern of distinct strings returns distinct StrIds
//   T5 — pool_get round-trips the original bytes (and NUL-terminates)
//   T6 — Same-length distinct strings disambiguate (catches the old
//        memcpy-instead-of-memcmp bug)
//   T7 — Grow event: insert past the 70% load factor, every prior
//        intern remains resolvable to the same StrId
//   T8 — Large workload (thousands of unique strings) — no collisions
//        produce false dedup; pool_get returns each string verbatim
//
// Run via `make test-stringpool`.

#include "../src/support/data_structure/stringpool.h"

#include <stdbool.h>
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

static void test_init(void) {
    start("pool_init: empty-string sentinel + clear slot table");
    StringPool pool;
    pool_init(&pool, 16);

    bool ok = (pool.slot_count == 16);
    ok &= (pool.slot_used == 0);
    // Every slot starts as POOL_EMPTY (0xFFFFFFFF).
    for (size_t i = 0; i < pool.slot_count; i++) {
        ok &= (pool.slots[i] == 0xFFFFFFFFu);
    }

    // The empty-string sentinel block is at arena offset 0.
    const char *empty = pool_get(&pool, (StrId){.idx = 0});
    ok &= (empty != NULL);
    ok &= (strcmp(empty, "") == 0);

    finish(ok);
    pool_free(&pool);
}

static void test_empty_fast_path(void) {
    start("pool_intern(\"\") fast-paths to StrId{0}; no slot allocated");
    StringPool pool;
    pool_init(&pool, 16);

    size_t before = pool.slot_used;
    StrId id = pool_intern(&pool, "", 0);

    bool ok = (id.idx == 0);
    ok &= (pool.slot_used == before);

    finish(ok);
    pool_free(&pool);
}

static void test_dedup_same_string(void) {
    start("identical strings return the same StrId");
    StringPool pool;
    pool_init(&pool, 16);

    StrId a = pool_intern(&pool, "hello", 5);
    StrId b = pool_intern(&pool, "hello", 5);

    bool ok = (a.idx == b.idx);
    ok &= (a.idx != 0);
    ok &= (pool.slot_used == 1);  // one entry, no duplicates

    finish(ok);
    pool_free(&pool);
}

static void test_distinct_strings(void) {
    start("distinct strings return distinct StrIds");
    StringPool pool;
    pool_init(&pool, 16);

    StrId a = pool_intern(&pool, "alpha",  5);
    StrId b = pool_intern(&pool, "beta",   4);
    StrId c = pool_intern(&pool, "gamma",  5);

    bool ok = (a.idx != b.idx);
    ok &= (a.idx != c.idx);
    ok &= (b.idx != c.idx);
    ok &= (pool.slot_used == 3);

    finish(ok);
    pool_free(&pool);
}

static void test_round_trip(void) {
    start("pool_get returns the original bytes (NUL-terminated)");
    StringPool pool;
    pool_init(&pool, 16);

    const char *src = "the quick brown fox";
    StrId id = pool_intern(&pool, src, strlen(src));
    const char *got = pool_get(&pool, id);

    bool ok = (got != NULL);
    ok &= (strcmp(got, src) == 0);
    ok &= (got[strlen(src)] == '\0');

    finish(ok);
    pool_free(&pool);
}

static void test_same_length_distinct(void) {
    start("same-length distinct strings don't false-dedup");
    StringPool pool;
    pool_init(&pool, 16);

    // The original bug used memcpy instead of memcmp — any two strings
    // of equal length appeared "equal." This test catches it.
    StrId a = pool_intern(&pool, "foo", 3);
    StrId b = pool_intern(&pool, "bar", 3);
    StrId c = pool_intern(&pool, "baz", 3);

    bool ok = (a.idx != b.idx);
    ok &= (a.idx != c.idx);
    ok &= (b.idx != c.idx);

    // Round-trip preserves contents.
    ok &= (strcmp(pool_get(&pool, a), "foo") == 0);
    ok &= (strcmp(pool_get(&pool, b), "bar") == 0);
    ok &= (strcmp(pool_get(&pool, c), "baz") == 0);

    finish(ok);
    pool_free(&pool);
}

static void test_grow_preserves_entries(void) {
    start("growth past 70% LF preserves every prior StrId");
    StringPool pool;
    pool_init(&pool, 16);

    // Insert enough to trigger at least one grow (16 * 0.7 = ~11).
    char buf[16];
    StrId saved[64];
    for (int i = 0; i < 64; i++) {
        snprintf(buf, sizeof(buf), "name_%d", i);
        saved[i] = pool_intern(&pool, buf, strlen(buf));
    }

    // After 64 inserts, slot_count must have grown past 16.
    bool ok = (pool.slot_count > 16);

    // Every saved StrId still resolves to its original bytes.
    for (int i = 0; i < 64; i++) {
        snprintf(buf, sizeof(buf), "name_%d", i);
        const char *got = pool_get(&pool, saved[i]);
        if (strcmp(got, buf) != 0) { ok = false; break; }
    }

    // Re-interning produces the same StrIds (dedup survived the grow).
    for (int i = 0; i < 64; i++) {
        snprintf(buf, sizeof(buf), "name_%d", i);
        StrId again = pool_intern(&pool, buf, strlen(buf));
        if (again.idx != saved[i].idx) { ok = false; break; }
    }

    finish(ok);
    pool_free(&pool);
}

static void test_large_workload(void) {
    start("1M unique strings: no false dedup, all round-trip (O(n) intern)");
    StringPool pool;
    pool_init(&pool, 16);

    // 1,000,000 unique strings: directly exercises the formerly-O(n^2)
    // intern path (chunk-walk offset resolution). With the contiguous
    // buffer this must complete in well under a second.
    const int N = 1000000;
    StrId *ids = malloc(N * sizeof(StrId));
    char buf[32];

    for (int i = 0; i < N; i++) {
        snprintf(buf, sizeof(buf), "ident_%07d_xyz", i);
        ids[i] = pool_intern(&pool, buf, strlen(buf));
    }

    bool ok = (pool.slot_used == (size_t)N);

    // Each StrId resolves back to its original string.
    for (int i = 0; i < N; i++) {
        snprintf(buf, sizeof(buf), "ident_%07d_xyz", i);
        if (strcmp(pool_get(&pool, ids[i]), buf) != 0) {
            ok = false;
            break;
        }
    }

    // Cross-check: distinct StrIds for distinct content.
    // (Sample 100 random pairs.)
    for (int t = 0; t < 100; t++) {
        int i = t * 47 % N;
        int j = (t * 91 + 13) % N;
        if (i == j) continue;
        if (ids[i].idx == ids[j].idx) { ok = false; break; }
    }

    free(ids);
    finish(ok);
    pool_free(&pool);
}

// =====================================================================

int main(void) {
    printf("stringpool unit tests\n");

    test_init();
    test_empty_fast_path();
    test_dedup_same_string();
    test_distinct_strings();
    test_round_trip();
    test_same_length_distinct();
    test_grow_preserves_entries();
    test_large_workload();

    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
