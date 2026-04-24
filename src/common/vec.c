#include "./vec.h"
#include <stdlib.h>
#include <string.h>

void vec_init(Vec* vec, size_t element_size) {
    vec->data = NULL;
    vec->count = 0;
    vec->capacity = 0;
    vec->element_size = element_size;
    vec->arena = NULL;
}

void vec_init_in(Vec* vec, Arena* arena, size_t element_size) {
    vec->data = NULL;        // lazy allocate on first push
    vec->count = 0;
    vec->capacity = 0;
    vec->element_size = element_size;
    vec->arena = arena;
}

Vec* vec_new_in(Arena* arena, size_t element_size) {
    Vec* v = arena_alloc(arena, sizeof(Vec));
    vec_init_in(v, arena, element_size);
    return v;
}

void vec_push(Vec* vec, const void* element) {
    if (vec->count == vec->capacity) {
        size_t new_capacity = vec->capacity < 8 ? 8 : vec->capacity * 2;
        size_t new_size = new_capacity * vec->element_size;
        if (vec->arena) {
            void* new_data = arena_alloc(vec->arena, new_size);
            if (vec->data && vec->count > 0) {
                memcpy(new_data, vec->data, vec->count * vec->element_size);
            }
            vec->data = new_data;
        } else {
            vec->data = realloc(vec->data, new_size);
        }
        vec->capacity = new_capacity;
    }
    void* dest = (char*)vec->data + vec->count * vec->element_size;
    memcpy(dest, element, vec->element_size);
    vec->count++;
}

void* vec_get(Vec* vec, size_t index) {
    if (index >= vec->count) {
        return NULL; // Out of bounds
    }
    // Calculate the memory address of the requested element
    return (char*)vec->data + index * vec->element_size;
}

void vec_free(Vec* vec) {
    if (vec->data != NULL) {
        free(vec->data);
    }
    vec->data = NULL;
    vec->count = 0;
    vec->capacity = 0;
}
