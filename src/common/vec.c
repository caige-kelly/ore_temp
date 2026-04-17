#include "./vec.h"
#include <stdlib.h>
#include <string.h>

void vec_init(Vec* vec, size_t element_size) {
    vec->data = NULL;
    vec->count = 0;
    vec->capacity = 0;
    vec->element_size = element_size;
}

void vec_push(Vec* vec, const void* element) {
    if (vec->count == vec->capacity) {
        vec->capacity = vec->capacity < 8 ? 8 : vec->capacity * 2;
        vec->data = realloc(vec->data, vec->capacity * vec->element_size);
    }
    // Calculate the memory address for the new element
    void* dest = (char*)vec->data + vec->count * vec->element_size;
    // Copy the element data into the vector
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
