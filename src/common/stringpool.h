#ifndef STRINGPOOL_H
#define STRINGPOOL_H

#include <stddef.h>

// A simple arena allocator for fast memory allocation and deallocation.
typedef struct {
    char* data;      // Pointer to the start of the arena memory
    size_t used;     // Amount of memory currently used
    size_t capacity; // Total capacity of the arena
} StringPool;

void pool_init(StringPool* pool, size_t initial_capacity);
uint32_t pool_intern(StringPool* pool, const char* str);
void pool_free(StringPool* pool);
const char* pool_get(StringPool* pool, void* ptr);

#endif // STRINGPOOL_H