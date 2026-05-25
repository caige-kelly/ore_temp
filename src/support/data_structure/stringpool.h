#ifndef STRINGPOOL_H
#define STRINGPOOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// PRE-EXISTING COUPLING: StrId is defined in src/db/ids/ids.h, which
// makes this "support" header depend on Ore-specific types. Inherited
// from before the support/data_structure consolidation. To honor the
// red/green extraction contract this would either: (a) move the StrId
// definition into stringpool.h (its natural home — the pool produces
// StrIds), or (b) introduce a generic u32-id-with-pool primitive and
// have ids.h alias it. Deferred to library-extraction day.
//
// Not load-bearing for src/syntax/: that module uses inline token text
// (not StrId-interned), so the syntax library's extraction contract
// is satisfied even with this header coupled to ids.h.
#include "../../db/ids/ids.h"
#include "./arena.h"

typedef struct {
    // Contiguous, growable string-bytes store. StrId is a direct byte
    // offset into `buffer`, so offset→pointer is `buffer + id` (O(1)).
    // (Was a chunked bump Arena, whose offset→pointer was an O(chunks)
    // linear walk → O(n²) interning under random hash-probe access.)
    char  *buffer;
    size_t len;   // bytes used == offset of the next interned block
    size_t cap;   // allocated capacity of `buffer`

    // The transient index on the heap
    uint32_t* slots;
    size_t slot_count;
    size_t slot_used;
} StringPool;


void pool_init(StringPool *pool, size_t initial_slots);
void pool_free(StringPool *pool);
void pool_reserve_slots(StringPool *pool, size_t expected_more);
StrId pool_intern(StringPool *pool, const char *str, size_t len);

// Look up a previously-interned string WITHOUT interning on miss.
// Returns StrId{0} when the string isn't in the pool. Use for read-
// only lookups (e.g. path → SourceId) where a miss must NOT leak a
// fresh entry into the pool.
StrId pool_lookup(StringPool *pool, const char *str, size_t len);

const char* pool_get(StringPool *pool, StrId id) ;

#endif // STRINGPOOL_H
