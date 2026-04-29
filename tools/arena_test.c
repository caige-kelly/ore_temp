#include "common/arena.h"

#include <stddef.h>
#include <string.h>

int main(void) {
    Arena arena;
    arena_init(&arena, 32);

    char* first = arena_alloc(&arena, 16);
    if (!first) return 1;

    memset(first, 0x5a, 16);

    void* overflow = arena_alloc(&arena, 1024 * 1024);
    if (!overflow) return 2;

    for (size_t i = 0; i < 16; i++) {
        if (first[i] != 0x5a) return 3;
    }

    arena_reset(&arena);

    char* reused = arena_alloc(&arena, 16);
    if (!reused) return 4;
    if (reused != first) return 5;

    for (size_t i = 0; i < 16; i++) {
        if (reused[i] != 0) return 6;
    }

    arena_free(&arena);
    return 0;
}