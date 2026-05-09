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

// Same as hashmap_put, but aborts with a diagnostic on failure.
// Use this at sites where a silent dropped insert would corrupt
// invariants — typically when the value owns a query slot and a
// later lookup miss would cause the slot to be re-created (orphan
// the original, break cycle detection / invalidation). The only
// realistic failure here is allocator OOM during table grow, which
// is unrecoverable for a compiler anyway.
void hashmap_put_or_die(HashMap* map, uint64_t key, void* value,
                        const char* site);
void* hashmap_get(const HashMap* map, uint64_t key);
bool hashmap_contains(const HashMap* map, uint64_t key);
void hashmap_clear(HashMap* map);
void hashmap_free(HashMap* map);

// Iterate every occupied entry. The callback returns true to keep going,
// false to stop early. Order is implementation-defined.
typedef bool (*HashMapVisitor)(uint64_t key, void* value, void* user_data);
void hashmap_foreach(const HashMap* map, HashMapVisitor visit, void* user_data);

#endif // HASHMAP_H