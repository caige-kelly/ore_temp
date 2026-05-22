#ifndef ORE_DB_QUERY_INVALIDATE_H
#define ORE_DB_QUERY_INVALIDATE_H

#include <stdbool.h>
#include "query.h"

struct db;

typedef enum {
    DB_REVALIDATE_RECOMPUTE,
    DB_REVALIDATE_SKIP_RECOMPUTE,
} RevalidateResult;

RevalidateResult db_revalidate(struct db *s, QuerySlot *slot);
QuerySlot* db_locate_slot(struct db *s, QueryKind kind, uint64_t key);

#endif // ORE_DB_QUERY_INVALIDATE_H
