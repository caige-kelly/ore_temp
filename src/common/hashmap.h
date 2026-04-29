#ifndef HASHMAP_H
#define HASHMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"

// Open-addressed integer-key map for compiler indexes and caches.
// Values are borrowed; the map does not own or free them. NULL values are
// allowed, so use hashmap_contains() when NULL is a meaningful stored value.
// Deletion is intentionally omitted until a compiler use case needs it.
typedef struct {
    uint64_t key;
    void* value;
    bool occupied;
} HashMapEntry;

typedef struct {
    HashMapEntry* entries;
    size_t count;
    size_t capacity;
    Arena* arena;
} HashMap;

void hashmap_init(HashMap* map);
void hashmap_init_in(HashMap* map, Arena* arena);
HashMap* hashmap_new_in(Arena* arena);
bool hashmap_put(HashMap* map, uint64_t key, void* value);
void* hashmap_get(const HashMap* map, uint64_t key);
bool hashmap_contains(const HashMap* map, uint64_t key);
void hashmap_clear(HashMap* map);
void hashmap_free(HashMap* map);

#endif // HASHMAP_H