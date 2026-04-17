#ifndef VEC_H
#define VEC_H

#include <stddef.h>

// The generic vector struct.
// It should be treated as opaque; only interact with it via the functions.
typedef struct {
    void* data;
    size_t count;
    size_t capacity;
    size_t element_size;
} Vec;

// Function declarations
void vec_init(Vec* vec, size_t element_size);
void vec_push(Vec* vec, const void* element);
void* vec_get(Vec* vec, size_t index);
void vec_free(Vec* vec);

#endif // VEC_H
