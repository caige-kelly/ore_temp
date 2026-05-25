#include "arena.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static size_t align_size(size_t size) {
  if (size > SIZE_MAX - 7)
    return 0;
  return (size + 7) & ~(size_t)7;
}

static ArenaChunk *arena_chunk_new(size_t capacity) {
  if (capacity > SIZE_MAX - sizeof(ArenaChunk))
    return NULL;

  ArenaChunk *chunk = malloc(sizeof(ArenaChunk) + capacity);
  if (!chunk)
    return NULL;

  chunk->next = NULL;
  chunk->used = 0;
  chunk->capacity = capacity;
  return chunk;
}

static void arena_chunk_free_list(ArenaChunk *chunk) {
  while (chunk) {
    ArenaChunk *next = chunk->next;
    free(chunk);
    chunk = next;
  }
}

static size_t next_chunk_capacity(Arena *a, size_t required_size) {
  size_t capacity =
      a->current ? a->current->capacity : a->default_chunk_capacity;
  if (capacity < a->default_chunk_capacity)
    capacity = a->default_chunk_capacity;
  if (capacity == 0)
    capacity = 8;

  while (capacity < required_size) {
    if (capacity > SIZE_MAX / 2)
      return required_size;
    capacity *= 2;
  }

  return capacity;
}

static int arena_contains_chunk(Arena *a, ArenaChunk *target) {
  if (!a || !target)
    return 0;

  for (ArenaChunk *chunk = a->first; chunk; chunk = chunk->next) {
    if (chunk == target)
      return 1;
  }

  return 0;
}

void arena_init(Arena *a, size_t default_chunk_capacity) {
  if (!a)
    return;
  memset(a, 0, sizeof(*a));
  a->default_chunk_capacity = default_chunk_capacity;
  a->generation = 1; // 0 is reserved for "no borrowed arena" stamps
}

size_t arena_total_used(Arena *a) {
  if (!a || !a->current)
    return 0;

  return a->total_prev_capacity + a->current->used;
}

void *arena_get_ptr(Arena *a, size_t offset) {
  if (!a)
    return NULL;

  // Fast path: the cached chunk's range covers this offset. Hit rate is
  // near 100% on intern-pool ip_key probe sequences, which read adjacent
  // payloads in the same chunk over and over. One compare and we're done.
  if (a->cached_chunk && offset >= a->cached_base &&
      offset < a->cached_base + a->cached_chunk->used) {
    return a->cached_chunk->data + (offset - a->cached_base);
  }

  // Slow path: walk chunks linearly. Update the cache on hit so the next
  // call short-circuits.
  size_t current_base = 0;
  ArenaChunk *c = a->first;

  while (c) {
    if (offset >= current_base && offset < current_base + c->used) {
      size_t local_offset = offset - current_base;
      a->cached_chunk = c;
      a->cached_base = current_base;
      return c->data + local_offset;
    }

    current_base += c->used;
    c = c->next;
  }

  return NULL; // Offset out of bounds
}

void *arena_alloc(Arena *a, size_t size) {
  void *ptr = arena_alloc_raw(a, size);
  if (ptr)
    memset(ptr, 0, align_size(size));
  return ptr;
}

void *arena_alloc_raw(Arena *a, size_t size) {
  if (!a)
    return NULL;

  size = align_size(size ? size : 8);
  // If we have no chunk, or the current one is full, we grow.
  if (!a->current || size > a->current->capacity - a->current->used) {
    // allocate a page as the default chunk size
    size_t default_cap =
        a->default_chunk_capacity ? a->default_chunk_capacity : 4096;
    size_t capacity =
        next_chunk_capacity(a, size > default_cap ? size : default_cap);

    ArenaChunk *chunk = arena_chunk_new(capacity);
    if (!chunk)
      return NULL;

    if (a->current) {
      a->total_prev_capacity += a->current->used;
      a->current->next = chunk;
    } else {
      a->first = chunk;
    }
    a->current = chunk;

    // Ensure default_chunk_capacity is set for future growths
    if (!a->default_chunk_capacity)
      a->default_chunk_capacity = default_cap;
  }

  void *ptr = a->current->data + a->current->used;
  a->current->used += size;
  return ptr;
}

ArenaMark arena_mark(Arena *a) {
  ArenaMark mark = {0};
  if (!a || !a->current)
    return mark;

  mark.chunk = a->current;
  mark.used = a->current->used;
  mark.total_prev_capacity = a->total_prev_capacity;
  return mark;
}

void arena_reset_to(Arena *a, ArenaMark mark) {
  // Validation checks
  if (!a || !mark.chunk)
    return;
  if (!arena_contains_chunk(a, mark.chunk))
    return;
  if (mark.used > mark.chunk->capacity)
    return;

  ArenaChunk *overflow = mark.chunk->next;
  mark.chunk->next = NULL;
  mark.chunk->used = mark.used;

  // restore the state
  a->total_prev_capacity = mark.total_prev_capacity;
  a->current = mark.chunk;

  // The cached chunk might be one of the freed overflow chunks; invalidate
  // unconditionally — cheaper than checking, and reset_to is cold.
  a->cached_chunk = NULL;
  a->cached_base = 0;

  // Bump the generation: any pointer borrowed from this arena before the
  // reset is now stale (see IpKey.src_gen).
  a->generation++;

  arena_chunk_free_list(overflow);
}

void arena_reset(Arena *a) {
  if (!a || !a->first)
    return;

  ArenaChunk *overflow = a->first->next;
  a->first->next = NULL;
  a->first->used = 0;
  a->current = a->first;
  a->total_prev_capacity = 0;

  a->cached_chunk = NULL;
  a->cached_base = 0;

  // Bump the generation: any pointer borrowed from this arena before the
  // reset is now stale (see IpKey.src_gen).
  a->generation++;

  arena_chunk_free_list(overflow);
}

void arena_free(Arena *a) {
  if (!a)
    return;

  arena_chunk_free_list(a->first);
  a->first = NULL;
  a->current = NULL;
  a->total_prev_capacity = 0;
  a->default_chunk_capacity = 0;

  a->cached_chunk = NULL;
  a->cached_base = 0;
}