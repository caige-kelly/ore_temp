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
    size_t total_prev_capacity;
} Arena;

typedef struct {
    ArenaChunk* chunk;
    size_t used;
    size_t total_prev_capacity;
} ArenaMark;

ArenaMark arena_mark(Arena* a);

// memsets to 0, slightly less performant
void* arena_alloc(Arena* a, size_t size);

// unitialized buffer; use for strings, file buffers, etc.
void* arena_alloc_raw(Arena* a, size_t size);

size_t arena_total_used(Arena* a);
void* arena_get_ptr(Arena* a, size_t offset);

void arena_reset_to(Arena* a, ArenaMark mark);
void arena_reset(Arena* a);
void arena_free(Arena* a);

#endif //ARENA_H