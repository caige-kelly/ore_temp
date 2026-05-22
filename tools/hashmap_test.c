#include "../src/db/storage/hashmap.h"

#include <stdint.h>

static int test_heap_map(void) {
    HashMap map;
    hashmap_init(&map);

    int zero = 11;
    if (!hashmap_put(&map, 0, &zero)) return 1;
    if (!hashmap_contains(&map, 0)) return 2;
    if (hashmap_get(&map, 0) != &zero) return 3;

    int values[512];
    for (uint64_t i = 1; i <= 512; i++) {
        values[i - 1] = (int)(i * 3);
        if (!hashmap_put(&map, i * 17, &values[i - 1])) return 4;
    }

    if (map.count != 513) return 5;

    for (uint64_t i = 1; i <= 512; i++) {
        int* got = hashmap_get(&map, i * 17);
        if (!got || *got != (int)(i * 3)) return 6;
    }

    int replacement = 99;
    if (!hashmap_put(&map, 17, &replacement)) return 7;
    if (map.count != 513) return 8;
    if (hashmap_get(&map, 17) != &replacement) return 9;

    if (!hashmap_put(&map, 9001, NULL)) return 10;
    if (!hashmap_contains(&map, 9001)) return 11;
    if (hashmap_get(&map, 9001) != NULL) return 12;

    hashmap_clear(&map);
    if (map.count != 0) return 13;
    if (hashmap_contains(&map, 17)) return 14;

    hashmap_free(&map);
    return 0;
}

static int test_remove(void) {
    HashMap map;
    hashmap_init(&map);

    // Build a small map with intentional collisions to exercise the
    // backward-shift cleanup. Use keys that share a low-bit hash so
    // they pile into one cluster.
    int v[16];
    for (uint64_t i = 0; i < 16; i++) {
        v[i] = (int)i;
        if (!hashmap_put(&map, i, &v[i])) return 30;
    }
    if (map.count != 16) return 31;

    // Remove a middle key. All others must still resolve.
    if (!hashmap_remove(&map, 7)) return 32;
    if (hashmap_contains(&map, 7)) return 33;
    if (map.count != 15) return 34;
    for (uint64_t i = 0; i < 16; i++) {
        if (i == 7) continue;
        int* got = hashmap_get(&map, i);
        if (!got || *got != (int)i) return 35;
    }

    // Removing a non-present key returns false and doesn't perturb count.
    if (hashmap_remove(&map, 9999)) return 36;
    if (map.count != 15) return 37;

    // Re-insert at the removed slot; verify both presence and value.
    int seven_again = 777;
    if (!hashmap_put(&map, 7, &seven_again)) return 38;
    if (map.count != 16) return 39;
    if (hashmap_get(&map, 7) != &seven_again) return 40;

    // Remove ALL keys; map should report empty afterwards. Stress-tests
    // the backward-shift loop's termination across many deletions.
    for (uint64_t i = 0; i < 16; i++) {
        if (!hashmap_remove(&map, i)) return 41;
    }
    if (map.count != 0) return 42;
    for (uint64_t i = 0; i < 16; i++) {
        if (hashmap_contains(&map, i)) return 43;
    }

    hashmap_free(&map);
    return 0;
}

static int test_arena_map(void) {
    Arena arena;
    arena_init(&arena, 128);

    HashMap* map = hashmap_new_in(&arena);
    if (!map) return 20;

    int values[300];
    for (uint64_t i = 0; i < 300; i++) {
        values[i] = (int)(i + 1000);
        if (!hashmap_put(map, i + 10000, &values[i])) return 21;
    }

    for (uint64_t i = 0; i < 300; i++) {
        int* got = hashmap_get(map, i + 10000);
        if (!got || *got != (int)(i + 1000)) return 22;
    }

    if (hashmap_contains(map, 9999)) return 23;

    hashmap_free(map);
    arena_free(&arena);
    return 0;
}

int main(void) {
    int result = test_heap_map();
    if (result != 0) return result;

    result = test_remove();
    if (result != 0) return result;

    result = test_arena_map();
    if (result != 0) return result;

    return 0;
}