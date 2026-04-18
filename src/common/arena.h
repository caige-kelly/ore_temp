#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>

typedef struct {
    char* data;
    size_t used;
    size_t capacity;
} Arena;

void arena_init(Arena* a, size_t initial_capacity);
void* arena_alloc(Arena* a, size_t size);
void arena_free(Arena* a);

#endif //ARENA_H