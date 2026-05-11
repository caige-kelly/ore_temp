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

static HashMapEntry *entries_alloc(HashMap *map, size_t capacity) {
  if (capacity > SIZE_MAX / sizeof(HashMapEntry))
    return NULL;

  size_t bytes = capacity * sizeof(HashMapEntry);
  if (map->arena)
    return arena_alloc(map->arena, bytes);
  return calloc(capacity, sizeof(HashMapEntry));
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

static void insert_existing(HashMapEntry *entries, size_t capacity,
                            uint64_t key, void *value) {
  size_t mask = capacity - 1;
  size_t index = (size_t)hash_u64(key) & mask;

  while (entries[index].occupied) {
    index = (index + 1) & mask;
  }

  entries[index].key = key;
  entries[index].value = value;
  entries[index].occupied = true;
}

static bool hashmap_grow(HashMap *map, size_t min_capacity) {
  size_t capacity = next_capacity(map->capacity, min_capacity);
  if (capacity == 0)
    return false;

  HashMapEntry *entries = entries_alloc(map, capacity);
  if (!entries)
    return false;

  for (size_t i = 0; i < map->capacity; i++) {
    HashMapEntry *entry = &map->entries[i];
    if (entry->occupied)
      insert_existing(entries, capacity, entry->key, entry->value);
  }

  if (!map->arena)
    free(map->entries);
  map->entries = entries;
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

  while (map->entries[index].occupied) {
    if (map->entries[index].key == key) {
      map->entries[index].value = value;
      return true;
    }
    index = (index + 1) & mask;
  }

  map->entries[index].key = key;
  map->entries[index].value = value;
  map->entries[index].occupied = true;
  map->count++;
  return true;
}

void *hashmap_get(const HashMap *map, uint64_t key) {
  if (!map || !map->entries || map->capacity == 0)
    return NULL;

  size_t mask = map->capacity - 1;
  size_t index = (size_t)hash_u64(key) & mask;

  while (map->entries[index].occupied) {
    if (map->entries[index].key == key)
      return map->entries[index].value;
    index = (index + 1) & mask;
  }

  return NULL;
}

bool hashmap_contains(const HashMap *map, uint64_t key) {
  if (!map || !map->entries || map->capacity == 0)
    return false;

  size_t mask = map->capacity - 1;
  size_t index = (size_t)hash_u64(key) & mask;

  while (map->entries[index].occupied) {
    if (map->entries[index].key == key)
      return true;
    index = (index + 1) & mask;
  }

  return false;
}

void hashmap_clear(HashMap *map) {
  if (!map || !map->entries)
    return;
  memset(map->entries, 0, map->capacity * sizeof(HashMapEntry));
  map->count = 0;
}

bool hashmap_remove(HashMap *map, uint64_t key) {
  if (!map || !map->entries || map->capacity == 0)
    return false;

  size_t mask = map->capacity - 1;

  // Locate the entry by linear probe (same probe sequence as
  // hashmap_get / hashmap_put).
  size_t i = (size_t)hash_u64(key) & mask;
  while (map->entries[i].occupied) {
    if (map->entries[i].key == key)
      break;
    i = (i + 1) & mask;
  }
  if (!map->entries[i].occupied || map->entries[i].key != key)
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
    map->entries[i].occupied = false;
    map->entries[i].key = 0;
    map->entries[i].value = NULL;

    size_t j = i;
    for (;;) {
      j = (j + 1) & mask;
      if (!map->entries[j].occupied) {
        map->count--;
        return true;
      }
      size_t r = (size_t)hash_u64(map->entries[j].key) & mask;
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
    map->entries[i] = map->entries[j];
    i = j;
  }
}

void hashmap_foreach(const HashMap *map, HashMapVisitor visit,
                     void *user_data) {
  if (!map || !visit || !map->entries)
    return;
  for (size_t i = 0; i < map->capacity; i++) {
    const HashMapEntry *entry = &map->entries[i];
    if (!entry->occupied)
      continue;
    if (!visit(entry->key, entry->value, user_data))
      return;
  }
}

void hashmap_free(HashMap *map) {
  if (!map)
    return;
  if (!map->arena)
    free(map->entries);
  map->entries = NULL;
  map->count = 0;
  map->capacity = 0;
  map->arena = NULL;
}