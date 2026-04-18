#include "stringpool.h"

void pool_init(StringPool* pool, size_t initial_capacity) {
    pool->data = malloc(initial_capacity);
    pool->used = 0;
    pool->capacity = initial_capacity;
}

uint32_t pool_intern(StringPool* pool, const char* str) {
    size_t len = strlen(str) + 1; // Include null terminator
    if (pool->used + len > pool->capacity) {
        // Double the capacity until it can fit the new string
        while (pool->used + len > pool->capacity) {
            pool->capacity *= 2;
        }
        pool->data = realloc(pool->data, pool->capacity);
    }
    uint32_t id = pool->used; // The ID is the offset in the data
    memcpy(pool->data + pool->used, str, len);
    pool->used += len;
    return id;
}

const char* pool_get(StringPool* pool, void* ptr) {
    if ((char*)ptr < pool->data || (char*)ptr >= pool->data + pool->used) {
        return NULL; // Invalid pointer
    }
    return (const char*)pool->data + (const char*)ptr -> string_id;