#include "stringpool.h"
#include <stdlib.h>
#include <string.h>

#define POOL_EMPTY 0xFFFFFFFFu

// FNV-1a 32-bit
static uint32_t hash_bytes(const char *s, size_t len) {
  uint32_t h = 0x811c9dc5u;
  for (size_t i = 0; i < len; i++) {
    h ^= (unsigned char)s[i];
    h *= 0x01000193u;
  }
  return h;
}

static void slots_init(StringPool *pool, size_t count) {
  pool->slot_count = count;
  pool->slot_used = 0;
  pool->slots = malloc(count * sizeof(uint32_t));
  for (size_t i = 0; i < count; i++)
    pool->slots[i] = POOL_EMPTY;
}

// Find the slot index for (str, len). On hit, *out_id is set to the
// existing id. On miss, returns the empty slot to insert into.
static size_t find_slot(StringPool *pool, const char *str, size_t len,
                        uint32_t hash, uint32_t *out_id) {
  size_t mask = pool->slot_count - 1;
  size_t i = hash & mask;
  *out_id = POOL_EMPTY;
  while (1) {
    uint32_t id = pool->slots[i];
    if (id == POOL_EMPTY) {
      return i;
    }
    // Compare content. The stored string is null-terminated, so we
    // require both length match and byte match.
    const char *existing = pool->data + id;
    if (strlen(existing) == len && memcmp(existing, str, len) == 0) {
      *out_id = id;
      return i;
    }
    i = (i + 1) & mask;
  }
}

static void slots_grow(StringPool *pool) {
  uint32_t *old_slots = pool->slots;
  size_t old_count = pool->slot_count;

  slots_init(pool, old_count * 2);

  // Re-hash existing entries.
  for (size_t i = 0; i < old_count; i++) {
    uint32_t id = old_slots[i];
    if (id == POOL_EMPTY)
      continue;
    const char *existing = pool->data + id;
    size_t len = strlen(existing);
    uint32_t h = hash_bytes(existing, len);
    uint32_t found_id;
    size_t slot = find_slot(pool, existing, len, h, &found_id);
    // found_id should be POOL_EMPTY since we just rebuilt.
    (void)found_id;
    pool->slots[slot] = id;
    pool->slot_used++;
  }

  free(old_slots);
}

void pool_init(StringPool *pool, size_t initial_capacity) {
  pool->data = malloc(initial_capacity);
  pool->used = 0;
  pool->capacity = initial_capacity;
  slots_init(pool, 256);
}

void pool_free(StringPool *pool) {
  free(pool->data);
  free(pool->slots);
  pool->data = NULL;
  pool->slots = NULL;
  pool->used = 0;
  pool->capacity = 0;
  pool->slot_count = 0;
  pool->slot_used = 0;
}

uint32_t pool_intern(StringPool *pool, const char *str, size_t len) {
  // Grow the slot table if load factor would exceed ~70%.
  if ((pool->slot_used + 1) * 10 > pool->slot_count * 7) {
    slots_grow(pool);
  }

  uint32_t h = hash_bytes(str, len);
  uint32_t found_id;
  size_t slot = find_slot(pool, str, len, h, &found_id);

  if (found_id != POOL_EMPTY) {
    return found_id; // already interned
  }

  // Append the bytes to data and record id == offset.
  if (pool->used + len + 1 > pool->capacity) {
    while (pool->used + len + 1 > pool->capacity) {
      pool->capacity *= 2;
    }
    pool->data = realloc(pool->data, pool->capacity);
  }

  uint32_t id = (uint32_t)pool->used;
  memcpy(pool->data + pool->used, str, len);
  pool->data[pool->used + len] = '\0';
  pool->used += len + 1;

  pool->slots[slot] = id;
  pool->slot_used++;

  return id;
}

const char *pool_get(StringPool *pool, uint32_t id, size_t len) {
  if (!pool || id >= pool->used || len > pool->used - id) {
    return NULL;
  }
  return (const char *)(pool->data + id);
}
