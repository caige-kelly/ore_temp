#ifndef ORE_DB_STORAGE_VEC_H
#define ORE_DB_STORAGE_VEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "./arena.h"

// =====================================================================
// Vec — typed dynamic array with two distinct backing flavors.
//
// Long-lived growing columns (SoA defs/scopes/modules/sources, query
// stack, etc.) want true realloc semantics. Initialize via vec_init —
// the Vec owns a malloc'd buffer; realloc handles growth in-place or
// by copy-and-free. No memory leak on grow. Call vec_free at teardown.
//
// Parse-time / per-revision tables (the Big Four side-tables, body
// stores, scope decl pools) live in an arena and have a known upper-
// bound capacity. Initialize via vec_init_in_arena(arena, max_count) —
// the Vec is a typed view over a single arena slab; vec_push asserts
// if count would exceed capacity. Storage is reclaimed when the arena
// itself is reset/freed; vec_free is a no-op.
//
// The old vec_init_in API (growing-arena-backed) leaked memory on
// every doubling — arena couldn't reclaim the old buffer. Replaced by
// these two patterns.
//
// Both flavors share the same access API:
//   vec_get(v, i)   - bounds-checked O(1) read, returns borrowed pointer
//   vec_push(v, e)  - append by copy
//   vec_push_zero(v)- append a zero-filled element
//   vec_free(v)     - release malloc backing (no-op for arena flavor)
// =====================================================================

typedef struct {
    void   *data;
    size_t  count;
    size_t  capacity;
    size_t  element_size;
    Arena  *arena;     // NULL = malloc-backed (growable via realloc)
                       // non-NULL = arena-backed (fixed capacity)
} Vec;

// Malloc-backed growable Vec. data starts NULL; first push allocates.
void vec_init(Vec *v, size_t element_size);

// Arena-backed fixed-capacity Vec. Allocates max_count * element_size
// bytes from `arena` in one shot. vec_push beyond max_count asserts.
void vec_init_in_arena(Vec *v, Arena *arena, size_t max_count,
                       size_t element_size);

// Append a copy of *element. Grows (malloc) or asserts (arena) on
// capacity exhaustion.
void vec_push(Vec *v, const void *element);

// Ensure a malloc-backed Vec can hold >= min_capacity elements without
// further realloc. No-op for arena-backed Vecs and when already large
// enough. Does not change count — a capacity hint, not a resize.
void vec_reserve(Vec *v, size_t min_capacity);

// Append a zero-filled element. Same growth/assert behavior.
void vec_push_zero(Vec *v);

// Return a borrowed pointer to element[i]. Borrow is invalidated by
// any subsequent push on a malloc-backed Vec (realloc may move data).
// Returns NULL if i >= count.
void *vec_get(Vec *v, size_t index);

// Reset count to 0 without releasing the backing buffer. For
// malloc-backed Vecs, the buffer stays allocated for reuse; for
// arena-backed Vecs the slab is also retained.
void vec_clear(Vec *v);

// Release malloc-backed storage. No-op for arena-backed Vecs (the
// caller is expected to reset or free the arena instead).
void vec_free(Vec *v);

#endif // ORE_DB_STORAGE_VEC_H
