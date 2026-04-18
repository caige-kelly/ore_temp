#include "stringpool.h"
#include <stdlib.h>
#include <string.h>


void pool_init(StringPool* pool, size_t initial_capacity) {
    pool->data = malloc(initial_capacity);
    pool->used = 0;
    pool->capacity = initial_capacity;
}

void pool_free(StringPool* pool) {
    free(pool->data);
    pool->data = NULL;
    pool->used = 0;
    pool->capacity = 0;
}

uint32_t pool_intern(StringPool* pool, const char* str, size_t len) {
    if (pool->used + len + 1> pool->capacity) {
        // Double the capacity until it can fit the new string
        while (pool->used + len + 1 > pool->capacity) {
            pool->capacity *= 2;
        }
        pool->data = realloc(pool->data, pool->capacity);
    }
    uint32_t id = pool->used; // The ID is the offset in the data
    memcpy(pool->data + pool->used, str, len);
    pool->data[pool->used + len] = '\0'; // Null-terminate the string
    pool->used += len + 1; // Account for the null terminator
    return id;
}

const char* pool_get(StringPool* pool, uint32_t id, size_t len) {
    if (id + len > pool->used) {
        return NULL; // Invalid ID or length
    }
    return (const char*)(pool->data + id);
}