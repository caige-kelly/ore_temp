#ifndef STRINGPOOL_H
#define STRINGPOOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../ids/ids.h"
#include "./arena.h"

typedef struct {
    Arena pool_mem; 

    // The transient index on the heap
    uint32_t* slots;
    size_t slot_count;
    size_t slot_used;
} StringPool;


void pool_init(StringPool *pool, size_t initial_slots);
void pool_free(StringPool *pool);
StrId pool_intern(StringPool *pool, const char *str, size_t len);
const char* pool_get(StringPool *pool, StrId id) ;

#endif // STRINGPOOL_H
