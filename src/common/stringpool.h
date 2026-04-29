#ifndef STRINGPOOL_H
#define STRINGPOOL_H

#include <stddef.h>
#include <stdint.h>

// A deduplicating string interner.
//
// pool_intern returns the same id for two byte-identical strings, so id
// equality == content equality. Store ids in long-lived structures.
// Pointers returned by pool_get are borrowed and may be invalidated by a
// later pool_intern, because the backing byte buffer can grow.
typedef struct {
    char* data;
    size_t used;
    size_t capacity;

    // Dedup index: open-addressing hash table mapping content -> id.
    // Each slot stores the id (offset into data) or POOL_EMPTY.
    uint32_t* slots;
    size_t slot_count;     // power of two
    size_t slot_used;
} StringPool;

void pool_init(StringPool* pool, size_t initial_capacity);
uint32_t pool_intern(StringPool* pool, const char* str, size_t len);
void pool_free(StringPool* pool);

// Returns a borrowed pointer into the pool. Do not retain it across pool_intern.
const char* pool_get(StringPool* pool, uint32_t id, size_t len);

#endif // STRINGPOOL_H
