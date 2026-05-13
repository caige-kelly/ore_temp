#include "../../db/storage/stringpool.h"
#include <stdlib.h>
#include <string.h>

#define POOL_EMPTY 0xFFFFFFFFu

// FNV-1a 32-bit; TODO: if bottleneck look into XXHash3 or WyHash.
static uint32_t hash_bytes(const char *s, size_t len) {
  uint32_t h = 0x811c9dc5u;
  for (size_t i = 0; i < len; i++) {
    h ^= (unsigned char)s[i];
    h *= 0x01000193u;
  }
  return h;
}

static void slots_init(StringPool *pool, size_t count) {
  pool->slot_count = count; // Must be power of 2 for mask
  pool->slot_used = 0;

  // malloc instead of arena because hash table need to be replaced as pool grows
  pool->slots = malloc(count * sizeof(uint32_t));
  memset(pool->slots, 0xFF, count * sizeof(uint32_t));
}

// Helper to turn ID into pointer in arena
// StrId.idx is the total offset across all chunks
static const char* pool_get_ptr(StringPool* pool, uint32_t id) {
  return arena_get_ptr(&pool->pool_mem, id);
}

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

    // Get pointer to header
    // we store: [uint32_t length][char...][\0]
    const char *entry_base = pool_get_ptr(pool, id);
    uint32_t existing_len;
    memcpy(&existing_len, entry_base, sizeof(uint32_t));

    // len check
    if (existing_len == (uint32_t)len) {
      const char *existing_str = entry_base + sizeof(uint32_t);
      if (memcpy((void*)existing_str, str, len) == 0)
        *out_id = id;
        return i;
    }
  }

  // Linear probing
  i = (i + 1) & mask;
}

static void slots_grow(StringPool *pool) {
  uint32_t *old_slots = pool->slots;
  size_t old_count = pool->slot_count;

  // allocate new table; double size
  slots_init(pool, old_count * 2);

  // hash existing entires
  for (size_t i = 0; i < old_count; i++) {
    uint32_t id = old_slots[i];
    if (id == POOL_EMPTY) continue;

    // find start of the [Lenght][Bytes] block
    const char *entry_base = pool_get_ptr(pool, id);

    // Extract length
    uint32_t len;
    memcpy(&len, entry_base, sizeof(uint32_t));
    const char *str = entry_base + sizeof(uint32_t);

    // calculate hash and find new slot
    uint32_t h = hash_bytes(str, len)
    uint32_t dummy_id;
    size_t new_slot = find_slot(pool, str, len, h, &dummy_id);

    pool->slots[new_slot] = id;
    pool->slot_used++;
  }

  // clean up the old table
  free(old_slots);
}

void pool_init(StringPool *pool, size_t initial_slots) {
  // 1. Initialize the index on the heap (malloc)
  // This part is transient and will be re-allocated during growth.
  slots_init(pool, initial_slots);

  // 2. Prepare the persistent storage (Arena)
  // We don't need to manually 'init' the arena if you're using 
  // the lazy-alloc pattern we discussed earlier.
  
  // 3. Register the 'NONE' string at offset 0
  // We store: [uint32_t length: 0][char: '\0']
  uint32_t zero_len = 0;
  void* header = arena_alloc(&pool->pool_mem, sizeof(uint32_t) + 1);
  memcpy(header, &zero_len, sizeof(uint32_t));
  ((char*)header)[sizeof(uint32_t)] = '\0';
}

void pool_free(StringPool *pool) {
  // 1. Free the heap-based index
  free(pool->slots);
  
  // 2. Free all chunks in the chained arena
  // This will internally walk the linked list of chunks and free() them.
  arena_free(&pool->pool_mem);

  // 3. Reset all metadata
  memset(pool, 0, sizeof(StringPool));
}

StrId pool_intern(StringPool *pool, const char *str, size_t len) {
  // 1. Check Load Factor and Grow if necessary (~70%)
  if ((pool->slot_used + 1) * 10 > pool->slot_count * 7) {
      slots_grow(pool);
  }

  // 2. Hash and Search
  uint32_t h = hash_bytes(str, len);
  uint32_t existing_id;
  size_t slot = find_slot(pool, str, len, h, &existing_id);

  if (existing_id != POOL_EMPTY) {
      return (StrId){ .idx = existing_id };
  }

  // 3. Allocate space for [Header (4 bytes)] + [String] + [Null Term]
  // We use arena_alloc_raw because we are about to memcpy everything anyway.
  uint32_t total_needed = (uint32_t)len + sizeof(uint32_t) + 1;
  
  // id is the global offset in the arena
  uint32_t id = (uint32_t)arena_total_used(&pool->pool_mem);
  
  void* ptr = arena_alloc_raw(&pool->pool_mem, total_needed);
  
  // 4. Write the Header (Length)
  uint32_t u32_len = (uint32_t)len;
  memcpy(ptr, &u32_len, sizeof(uint32_t));
  
  // 5. Write the String and Null Terminator
  char* dest_str = (char*)ptr + sizeof(uint32_t);
  memcpy(dest_str, str, len);
  dest_str[len] = '\0';

  // 6. Update the Index
  pool->slots[slot] = id;
  pool->slot_used++;

  return (StrId){ .idx = id };
}

const char* pool_get(StringPool *pool, StrId id) {
  if (id.idx == 0) return ""; // Safe return for NONE
  
  // Get pointer from arena using global offset
  const char* base = arena_get_ptr(&pool->pool_mem, id.idx);
  
  // Skip the 4-byte length header to get to the actual characters
  return base + sizeof(uint32_t);
}