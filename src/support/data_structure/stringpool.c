#include "../../db/storage/stringpool.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

// =====================================================================
// StringPool — content-deduped string interner.
//
// Storage layout (all in the chained arena `pool_mem`):
//   [u32 length][bytes...][\0]
//
//   The block's byte offset within the arena IS the StrId. Two strings
//   with identical content share the same offset (and therefore the
//   same StrId). The trailing \0 lets pool_get return a C string.
//
// Slot table (heap-allocated, rebuilt on growth):
//   Open-addressed; linear probe. Each slot holds either POOL_EMPTY or
//   the byte offset of the stored block. Indexed by `hash & mask` where
//   `mask = slot_count - 1` (slot_count must be a power of two).
//
// Empty string convention: StrId{0} is reserved as the empty string.
// pool_init seeds the arena at offset 0 with an empty block so any
// non-empty intern allocates after it (offset 0 stays reserved).
// pool_intern("") fast-paths to StrId{0} without touching the slot
// table.
// =====================================================================

#define POOL_EMPTY 0xFFFFFFFFu

// FNV-1a 32-bit. Cheap; adequate for short identifiers. Switch to
// xxh3 / wyhash if profiling shows hashing dominates.
static uint32_t hash_bytes(const char *s, size_t len) {
  uint32_t h = 0x811c9dc5u;
  for (size_t i = 0; i < len; i++) {
    h ^= (unsigned char)s[i];
    h *= 0x01000193u;
  }
  return h;
}

static const char *block_at(StringPool *pool, uint32_t id) {
  return (const char *)arena_get_ptr(&pool->pool_mem, id);
}

// Locate the slot for (str, len, hash). On HIT (string already present),
// returns the slot index with *out_id set to the existing StrId byte
// offset. On MISS (string not present), returns the insertion slot
// index with *out_id == POOL_EMPTY.
//
// Linear probe with cyclic wrap. The slot table must always have at
// least one empty slot (enforced by the load-factor check in
// pool_intern), so this loop is guaranteed to terminate.
static size_t find_slot(StringPool *pool, const char *str, size_t len,
                        uint32_t hash, uint32_t *out_id) {
  size_t mask = pool->slot_count - 1;
  size_t i = (size_t)hash & mask;

  for (;;) {
    uint32_t id = pool->slots[i];
    if (id == POOL_EMPTY) {
      *out_id = POOL_EMPTY;
      return i;
    }

    const char *entry = block_at(pool, id);
    uint32_t existing_len;
    memcpy(&existing_len, entry, sizeof(uint32_t));

    if (existing_len == (uint32_t)len) {
      const char *existing_str = entry + sizeof(uint32_t);
      if (memcmp(existing_str, str, len) == 0) {
        *out_id = id;
        return i;
      }
    }

    i = (i + 1) & mask;
  }
}

// Resize the slot table to `new_count` (power of two) and rehash every
// existing entry. Called when the load factor would exceed ~70%.
static void slots_resize(StringPool *pool, size_t new_count) {
  assert(new_count >= 16 && (new_count & (new_count - 1)) == 0 &&
         "stringpool: slot_count must be a power of two >= 16");

  uint32_t *old_slots = pool->slots;
  size_t old_count = pool->slot_count;

  pool->slots = malloc(new_count * sizeof(uint32_t));
  memset(pool->slots, 0xFF, new_count * sizeof(uint32_t));
  pool->slot_count = new_count;
  // slot_used count carries over — we're rehashing identical entries.

  size_t mask = new_count - 1;
  for (size_t i = 0; i < old_count; i++) {
    uint32_t id = old_slots[i];
    if (id == POOL_EMPTY)
      continue;

    const char *entry = block_at(pool, id);
    uint32_t len;
    memcpy(&len, entry, sizeof(uint32_t));
    const char *str = entry + sizeof(uint32_t);

    uint32_t h = hash_bytes(str, len);
    size_t j = (size_t)h & mask;
    while (pool->slots[j] != POOL_EMPTY)
      j = (j + 1) & mask;
    pool->slots[j] = id;
  }

  free(old_slots);
}

void pool_init(StringPool *pool, size_t initial_slots) {
  assert(initial_slots >= 16 && (initial_slots & (initial_slots - 1)) == 0 &&
         "pool_init: initial_slots must be a power of two >= 16");

  *pool = (StringPool){0};

  pool->slots = malloc(initial_slots * sizeof(uint32_t));
  memset(pool->slots, 0xFF, initial_slots * sizeof(uint32_t));
  pool->slot_count = initial_slots;
  pool->slot_used = 0;

  // Seed the arena's offset 0 with the empty-string block so any
  // subsequent intern allocates at offset >= the seeded block's size.
  // The seeded block is NOT registered in the slot table; lookups for
  // the empty string fast-path to StrId{0} in pool_intern / pool_get.
  uint32_t zero_len = 0;
  void *header = arena_alloc_raw(&pool->pool_mem, sizeof(uint32_t) + 1);
  memcpy(header, &zero_len, sizeof(uint32_t));
  ((char *)header)[sizeof(uint32_t)] = '\0';
}

void pool_free(StringPool *pool) {
  free(pool->slots);
  arena_free(&pool->pool_mem);
  memset(pool, 0, sizeof(StringPool));
}

StrId pool_intern(StringPool *pool, const char *str, size_t len) {
  // Empty-string fast path — StrId{0} by convention.
  if (len == 0)
    return (StrId){.idx = 0};

  // Load-factor check (~70%). Resize before insertion so we never
  // probe into a stretched table.
  if ((pool->slot_used + 1) * 10 > pool->slot_count * 7) {
    slots_resize(pool, pool->slot_count * 2);
  }

  uint32_t h = hash_bytes(str, len);
  uint32_t existing_id;
  size_t slot = find_slot(pool, str, len, h, &existing_id);

  if (existing_id != POOL_EMPTY) {
    return (StrId){.idx = existing_id};
  }

  // Insert: allocate [u32 len][bytes][\0] in the arena. The block's
  // byte offset is the StrId.
  uint32_t total = (uint32_t)len + sizeof(uint32_t) + 1;
  uint32_t offset = (uint32_t)arena_total_used(&pool->pool_mem);
  void *ptr = arena_alloc_raw(&pool->pool_mem, total);

  uint32_t u32_len = (uint32_t)len;
  memcpy(ptr, &u32_len, sizeof(uint32_t));
  char *dest = (char *)ptr + sizeof(uint32_t);
  memcpy(dest, str, len);
  dest[len] = '\0';

  pool->slots[slot] = offset;
  pool->slot_used++;

  return (StrId){.idx = offset};
}

const char *pool_get(StringPool *pool, StrId id) {
  if (id.idx == 0)
    return ""; // Empty-string fast path.

  const char *block = block_at(pool, id.idx);
  return block + sizeof(uint32_t);
}
