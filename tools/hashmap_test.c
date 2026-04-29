#include "common/hashmap.h"

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

    result = test_arena_map();
    if (result != 0) return result;

    return 0;
}