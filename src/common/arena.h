#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>

typedef struct ArenaChunk {
    struct ArenaChunk* next;
    size_t used;
    size_t capacity;
    char data[];
} ArenaChunk;

typedef struct {
    ArenaChunk* first;
    ArenaChunk* current;
    size_t default_chunk_capacity;
} Arena;

typedef struct {
    ArenaChunk* chunk;
    size_t used;
} ArenaMark;

void arena_init(Arena* a, size_t initial_capacity);
void* arena_alloc(Arena* a, size_t size);
ArenaMark arena_mark(Arena* a);
void arena_reset_to(Arena* a, ArenaMark mark);
void arena_reset(Arena* a);
void arena_free(Arena* a);

#endif //ARENA_H