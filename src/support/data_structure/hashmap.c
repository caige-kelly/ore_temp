#include "hashmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  HASHMAP_MIN_CAPACITY = 16,
  HASHMAP_MAX_LOAD_NUMERATOR = 7,
  HASHMAP_MAX_LOAD_DENOMINATOR = 10,
};

static uint64_t hash_u64(uint64_t key) {
  key ^= key >> 30;
  key *= 0xbf58476d1ce4e5b9ULL;
  key ^= key >> 27;
  key *= 0x94d049bb133111ebULL;
  key ^= key >> 31;
  return key;
}

// --- occupied bitset --------------------------------------------------------
// Emptiness lives in a bitset, not a key/value sentinel: key 0 and value
// NULL are both legal stored data.

static inline bool bit_get(const uint64_t *bits, size_t i) {
  return (bits[i >> 6] >> (i & 63)) & 1u;
}
static inline void bit_set(uint64_t *bits, size_t i) {
  bits[i >> 6] |= (uint64_t)1 << (i & 63);
}
static inline void bit_clear(uint64_t *bits, size_t i) {
  bits[i >> 6] &= ~((uint64_t)1 << (i & 63));
}
static inline size_t occupied_words(size_t capacity) {
  return (capacity + 63) / 64;
}

// The three columns of an open-addressed table. One backing block holds
// keys[capacity], values[capacity] and the occupied bitset back-to-back;
// `keys` is the block start, so freeing it frees the whole store.
typedef struct {
  uint64_t *keys;
  void **values;
  uint64_t *occupied;
} HashMapStore;

// Allocate (and zero) one block for a `capacity`-slot store. Zeroed so the
// occupied bitset starts all-clear. Returns false on OOM.
static bool store_alloc(Arena *arena, size_t capacity, HashMapStore *out) {
  size_t kbytes = capacity * sizeof(uint64_t);
  size_t vbytes = capacity * sizeof(void *);
  size_t obytes = occupied_words(capacity) * sizeof(uint64_t);
  size_t total = kbytes + vbytes + obytes;

  // arena_alloc and calloc both return zeroed memory.
  void *block = arena ? arena_alloc(arena, total) : calloc(1, total);
  if (!block)
    return false;

  out->keys = (uint64_t *)block;
  out->values = (void **)((char *)block + kbytes);
  out->occupied = (uint64_t *)((char *)block + kbytes + vbytes);
  return true;
}

static size_t next_capacity(size_t current, size_t min_capacity) {
  size_t capacity = HASHMAP_MIN_CAPACITY;
  if (current) {
    if (current > SIZE_MAX / 2)
      return 0;
    capacity = current * 2;
  }
  if (capacity < HASHMAP_MIN_CAPACITY)
    capacity = HASHMAP_MIN_CAPACITY;

  while (capacity < min_capacity) {
    if (capacity > SIZE_MAX / 2)
      return 0;
    capacity *= 2;
  }

  return capacity;
}

// Insert into a fresh store with no existing-key check (rehash path).
static void insert_existing(HashMapStore *st, size_t capacity, uint64_t key,
                            void *value) {
  size_t mask = capacity - 1;
  size_t index = (size_t)hash_u64(key) & mask;
  while (bit_get(st->occupied, index))
    index = (index + 1) & mask;
  st->keys[index] = key;
  st->values[index] = value;
  bit_set(st->occupied, index);
}

static bool hashmap_grow(HashMap *map, size_t min_capacity) {
  size_t capacity = next_capacity(map->capacity, min_capacity);
  if (capacity == 0)
    return false;

  HashMapStore ns;
  if (!store_alloc(map->arena, capacity, &ns))
    return false;

  for (size_t i = 0; i < map->capacity; i++) {
    if (bit_get(map->occupied, i))
      insert_existing(&ns, capacity, map->keys[i], map->values[i]);
  }

  if (!map->arena && map->keys)
    free(map->keys); // keys is the block start on the malloc path
  map->keys = ns.keys;
  map->values = ns.values;
  map->occupied = ns.occupied;
  map->capacity = capacity;
  return true;
}

static bool hashmap_needs_grow(HashMap *map) {
  if (map->capacity == 0)
    return true;
  if (map->count == SIZE_MAX)
    return true;

  size_t max_count = (map->capacity / HASHMAP_MAX_LOAD_DENOMINATOR) *
                     HASHMAP_MAX_LOAD_NUMERATOR;
  max_count += (map->capacity % HASHMAP_MAX_LOAD_DENOMINATOR) *
               HASHMAP_MAX_LOAD_NUMERATOR / HASHMAP_MAX_LOAD_DENOMINATOR;
  return map->count + 1 > max_count;
}

void hashmap_init(HashMap *map) {
  if (!map)
    return;
  *map = (HashMap){0};
}

void hashmap_init_in(HashMap *map, Arena *arena) {
  if (!map)
    return;
  *map = (HashMap){0};
  map->arena = arena;
}

HashMap *hashmap_new_in(Arena *arena) {
  if (!arena)
    return NULL;

  HashMap *map = arena_alloc(arena, sizeof(HashMap));
  if (!map)
    return NULL;
  hashmap_init_in(map, arena);
  return map;
}

void hashmap_put_or_die(HashMap *map, uint64_t key, void *value,
                        const char *site) {
  if (!hashmap_put(map, key, value)) {
    fprintf(stderr,
            "fatal: hashmap insert failed at %s "
            "(out of memory or capacity exhausted)\n",
            site ? site : "<unknown>");
    abort();
  }
}

bool hashmap_put(HashMap *map, uint64_t key, void *value) {
  if (!map)
    return false;

  if (hashmap_needs_grow(map) &&
      (map->capacity == SIZE_MAX || !hashmap_grow(map, map->capacity + 1))) {
    return false;
  }

  size_t mask = map->capacity - 1;
  size_t index = (size_t)hash_u64(key) & mask;

  while (bit_get(map->occupied, index)) {
    if (map->keys[index] == key) {
      map->values[index] = value;
      return true;
    }
    index = (index + 1) & mask;
  }

  map->keys[index] = key;
  map->values[index] = value;
  bit_set(map->occupied, index);
  map->count++;
  return true;
}

void *hashmap_get(const HashMap *map, uint64_t key) {
  if (!map || !map->keys || map->capacity == 0)
    return NULL;

  size_t mask = map->capacity - 1;
  size_t index = (size_t)hash_u64(key) & mask;

  while (bit_get(map->occupied, index)) {
    if (map->keys[index] == key)
      return map->values[index];
    index = (index + 1) & mask;
  }

  return NULL;
}

bool hashmap_contains(const HashMap *map, uint64_t key) {
  if (!map || !map->keys || map->capacity == 0)
    return false;

  size_t mask = map->capacity - 1;
  size_t index = (size_t)hash_u64(key) & mask;

  while (bit_get(map->occupied, index)) {
    if (map->keys[index] == key)
      return true;
    index = (index + 1) & mask;
  }

  return false;
}

void hashmap_clear(HashMap *map) {
  if (!map || !map->occupied)
    return;
  memset(map->occupied, 0, occupied_words(map->capacity) * sizeof(uint64_t));
  map->count = 0;
}

bool hashmap_remove(HashMap *map, uint64_t key) {
  if (!map || !map->keys || map->capacity == 0)
    return false;

  size_t mask = map->capacity - 1;

  // Locate the entry by linear probe (same probe sequence as
  // hashmap_get / hashmap_put).
  size_t i = (size_t)hash_u64(key) & mask;
  while (bit_get(map->occupied, i)) {
    if (map->keys[i] == key)
      break;
    i = (i + 1) & mask;
  }
  if (!bit_get(map->occupied, i) || map->keys[i] != key)
    return false;

  // Backward-shift cleanup. After erasing slot `i`, walk forward to
  // find any entry whose natural slot is at or before `i` (modulo
  // capacity) — those got "pushed past" `i` by an earlier collision
  // and can be moved back to keep the probe chain unbroken. Repeat
  // with the now-empty slot as the new "hole."
  //
  // Standard formulation (Knuth Vol 3, §6.4 exercise; common in
  // open-addressing hash table literature). The cyclic-range check
  // distinguishes the wrap-around case.
  for (;;) {
    bit_clear(map->occupied, i);
    map->keys[i] = 0;
    map->values[i] = NULL;

    size_t j = i;
    for (;;) {
      j = (j + 1) & mask;
      if (!bit_get(map->occupied, j)) {
        map->count--;
        return true;
      }
      size_t r = (size_t)hash_u64(map->keys[j]) & mask;
      // Move entry at j → i iff i is on the cyclic probe path from
      // r to j inclusive. Two cases: non-wrapping (j > i) and
      // wrapping (j < i, the chain crosses the capacity boundary).
      bool should_move;
      if (j > i)
        should_move = (r <= i) || (r > j);
      else
        should_move = (r <= i) && (r > j);
      if (should_move)
        break;
    }
    map->keys[i] = map->keys[j];
    map->values[i] = map->values[j];
    bit_set(map->occupied, i);
    i = j;
  }
}

void hashmap_foreach(const HashMap *map, HashMapVisitor visit,
                     void *user_data) {
  if (!map || !visit || !map->keys)
    return;
  for (size_t i = 0; i < map->capacity; i++) {
    if (!bit_get(map->occupied, i))
      continue;
    if (!visit(map->keys[i], map->values[i], user_data))
      return;
  }
}

void hashmap_free(HashMap *map) {
  if (!map)
    return;
  if (!map->arena && map->keys)
    free(map->keys); // keys is the block start
  map->keys = NULL;
  map->values = NULL;
  map->occupied = NULL;
  map->count = 0;
  map->capacity = 0;
  map->arena = NULL;
}
