#include "arena.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static size_t align_size(size_t size) {
    if (size > SIZE_MAX - 7) return 0;
    return (size + 7) & ~(size_t)7;
}

static ArenaChunk* arena_chunk_new(size_t capacity) {
    if (capacity > SIZE_MAX - sizeof(ArenaChunk)) return NULL;

    ArenaChunk* chunk = malloc(sizeof(ArenaChunk) + capacity);
    if (!chunk) return NULL;

    chunk->next = NULL;
    chunk->used = 0;
    chunk->capacity = capacity;
    return chunk;
}

static void arena_chunk_free_list(ArenaChunk* chunk) {
    while (chunk) {
        ArenaChunk* next = chunk->next;
        free(chunk);
        chunk = next;
    }
}

static size_t next_chunk_capacity(Arena* a, size_t required_size) {
    size_t capacity = a->current ? a->current->capacity : a->default_chunk_capacity;
    if (capacity < a->default_chunk_capacity) capacity = a->default_chunk_capacity;
    if (capacity == 0) capacity = 8;

    while (capacity < required_size) {
        if (capacity > SIZE_MAX / 2) return required_size;
        capacity *= 2;
    }

    return capacity;
}

void arena_init(Arena* a, size_t capacity) {
    if (!a) return;

    size_t aligned_capacity = align_size(capacity);
    if (capacity > 0 && aligned_capacity == 0) {
        *a = (Arena){0};
        return;
    }
    capacity = aligned_capacity ? aligned_capacity : 8;

    a->first = arena_chunk_new(capacity);
    a->current = a->first;
    a->default_chunk_capacity = capacity;
}

void* arena_alloc(Arena* a, size_t size) {
    if (!a) return NULL;

    size_t requested_size = size;
    size = align_size(size);
    if (requested_size > 0 && size == 0) return NULL;
    if (size == 0) size = 8;

    if (!a->current) {
        size_t capacity = next_chunk_capacity(a, size);
        a->first = arena_chunk_new(capacity);
        a->current = a->first;
        if (!a->current) return NULL;
    } else if (size > a->current->capacity - a->current->used) {
        size_t capacity = next_chunk_capacity(a, size);
        ArenaChunk* chunk = arena_chunk_new(capacity);
        if (!chunk) return NULL;

        if (a->current) {
            a->current->next = chunk;
        } else {
            a->first = chunk;
        }
        a->current = chunk;
    }

    void* ptr = a->current->data + a->current->used;
    memset(ptr, 0, size);
    a->current->used += size;
    return ptr;
}

void arena_reset(Arena* a) {
    if (!a || !a->first) return;

    ArenaChunk* overflow = a->first->next;
    a->first->next = NULL;
    a->first->used = 0;
    a->current = a->first;
    arena_chunk_free_list(overflow);
}

void arena_free(Arena* a) {
    if (!a) return;

    arena_chunk_free_list(a->first);
    a->first = NULL;
    a->current = NULL;
    a->default_chunk_capacity = 0;
}