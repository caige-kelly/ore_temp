#ifndef STRINGPOOL_H
#define STRINGPOOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../ids/ids.h"

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

#define STR_ID_NONE ((StrId){0})

static inline bool str_id_is_valid(StrId id) { return id.idx != 0; }
static inline bool str_id_eq(StrId a, StrId b) { return a.idx == b.idx; }

void pool_init(StringPool* pool, size_t initial_capacity);

// Intern `str` (length `len`) and return its StrId. Identical bytes
// always return the same StrId across calls on the same pool.
StrId pool_intern(StringPool* pool, const char* str, size_t len);

void pool_free(StringPool* pool);

// Returns a borrowed pointer into the pool. Do not retain it across
// pool_intern. `len` is informational; pass 0 if you don't need it.
const char* pool_get(StringPool* pool, StrId id, size_t len);

#endif // STRINGPOOL_H
