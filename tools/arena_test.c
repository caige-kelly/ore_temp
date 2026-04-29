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

    memset(reused, 0x3c, 16);

    ArenaMark outer = arena_mark(&arena);
    char* outer_alloc = arena_alloc(&arena, 8);
    if (!outer_alloc) return 7;
    memset(outer_alloc, 0x2a, 8);

    ArenaMark inner = arena_mark(&arena);
    char* inner_alloc = arena_alloc(&arena, 8);
    if (!inner_alloc) return 8;
    memset(inner_alloc, 0x1b, 8);

    void* inner_overflow = arena_alloc(&arena, 1024 * 1024);
    if (!inner_overflow) return 9;

    arena_reset_to(&arena, inner);
    char* inner_reused = arena_alloc(&arena, 8);
    if (!inner_reused) return 10;
    if (inner_reused != inner_alloc) return 11;
    for (size_t i = 0; i < 8; i++) {
        if (inner_reused[i] != 0) return 12;
    }

    arena_reset_to(&arena, outer);
    char* outer_reused = arena_alloc(&arena, 8);
    if (!outer_reused) return 13;
    if (outer_reused != outer_alloc) return 14;
    for (size_t i = 0; i < 8; i++) {
        if (outer_reused[i] != 0) return 15;
    }

    for (size_t i = 0; i < 16; i++) {
        if (reused[i] != 0x3c) return 16;
    }

    arena_free(&arena);
    return 0;
}