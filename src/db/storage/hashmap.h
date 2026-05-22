#ifndef HASHMAP_H
#define HASHMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"

// Open-addressed integer-key map for compiler indexes and caches.
// Values are borrowed; the map does not own or free them. NULL values are
// allowed, and key 0 is a legal key — emptiness is tracked by a separate
// `occupied` bitset, never by a key/value sentinel.
//
// SoA layout: keys / values / occupied are three parallel arrays sharing
// one backing block. A probe sequence scans the dense `keys[]` (8 keys per
// cache line) and only touches `values[]` on a hit.
//
// Deletion uses backward-shift (Robin Hood) cleanup — no tombstones, the
// probe chain stays compact. Cost: O(cluster length), constant in
// expectation under bounded load factor.
typedef struct {
    uint64_t *keys;      // [capacity]
    void    **values;    // [capacity]
    uint64_t *occupied;  // bitset, [(capacity + 63) / 64] words
    size_t    count;
    size_t    capacity;
    Arena    *arena;
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

// True once the map has live backing storage (after its first insert).
// A zero-initialized / hashmap_init'd map is not yet initialized.
static inline bool hashmap_is_initialized(const HashMap* map) {
    return map && map->keys != NULL;
}

// Remove `key` from the map. Returns true if the key was present and
// removed, false if it wasn't there. After return, no follow-up lookup
// can find this key (until/unless it's re-inserted). Uses backward-shift
// cleanup so subsequent probes for surviving keys remain O(1) and the
// probe chain stays compact (no tombstones).
bool hashmap_remove(HashMap* map, uint64_t key);

// Iterate every occupied entry. The callback returns true to keep going,
// false to stop early. Order is implementation-defined.
typedef bool (*HashMapVisitor)(uint64_t key, void* value, void* user_data);
void hashmap_foreach(const HashMap* map, HashMapVisitor visit, void* user_data);

#endif // HASHMAP_H
