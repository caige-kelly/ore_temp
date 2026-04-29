#ifndef VEC_H
#define VEC_H

#include <stddef.h>
#include "./arena.h"

// The generic vector struct.
// It should be treated as opaque; only interact with it via the functions.
typedef struct {
    void* data;
    size_t count;
    size_t capacity;
    size_t element_size;
    Arena* arena; // NULL = malloc/realloc mode; non-NULL = arena mode
} Vec;

// Function declarations
void vec_init(Vec* vec, size_t element_size);
void vec_push(Vec* vec, const void* element);

// Returns a borrowed element pointer. Do not retain it across vec_push on
// the same vector, because growth can move the backing storage.
void* vec_get(Vec* vec, size_t index);
void vec_free(Vec* vec);
void vec_init_in(Vec* vec, Arena* arena, size_t element_size);
Vec* vec_new_in(Arena* arena, size_t element_size);

#endif // VEC_H
