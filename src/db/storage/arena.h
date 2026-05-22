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

    // One-slot lookup cache for arena_get_ptr. Many consecutive lookups
    // hit offsets in the same chunk (e.g. the intern pool's ip_key probe
    // sequence reads adjacent payloads); caching (chunk, base) collapses
    // the linear chunk walk to one compare on the hit path.
    ArenaChunk* cached_chunk;
    size_t      cached_base;

    // Reset generation. Starts at 1 (arena_init); arena_reset and
    // arena_reset_to bump it. Lets a borrowed-pointer holder detect that
    // the arena it points into has since been reset out from under it —
    // see IpKey.src_gen and the assert in ip_get. Never 0 on a live
    // arena, so a 0 stamp reads as "no borrowed arena".
    unsigned    generation;
} Arena;

typedef struct {
    ArenaChunk* chunk;
    size_t used;
    size_t total_prev_capacity;
} ArenaMark;

// Initialize the arena with the given default chunk capacity. The
// arena will use this size for its first chunk and doubling on
// subsequent growth. Passing 0 falls back to a built-in default
// (4096). Always memset's the struct first — safe to call on
// uninitialized memory.
void arena_init(Arena* a, size_t default_chunk_capacity);

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