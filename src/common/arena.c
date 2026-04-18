#include "arena.h"
#include <stdlib.h>
#include <string.h>

void arena_init(Arena* a, size_t capacity) {
    a->data = malloc(capacity);
    a->used = 0;
    a->capacity = capacity;
}

void* arena_alloc(Arena* a, size_t size) {
    //Align to 8 bytes
    size = (size + 7) & ~7;
    if (a->used + size > a->capacity) {
        while (a->used + size > a -> capacity) {
            a->capacity += 2;
        }
        a->data = realloc(a->data, a->capacity);
    }
    void* ptr = a->data + a->used;
    memset(ptr, 0, size); // zero-init liek calloc
    a->used += size;
    return ptr;
}

void arena_free(Arena* a) {
    free(a->data);
    a->data = NULL;
    a->used = 0;
    a->capacity = 0;
}