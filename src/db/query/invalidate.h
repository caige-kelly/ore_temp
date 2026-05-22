#ifndef ORE_DB_QUERY_INVALIDATE_H
#define ORE_DB_QUERY_INVALIDATE_H

#include <stdbool.h>
#include "query.h"

struct db;

typedef enum {
    DB_REVALIDATE_RECOMPUTE,
    DB_REVALIDATE_SKIP_RECOMPUTE,
} RevalidateResult;

RevalidateResult db_revalidate(struct db *s, QuerySlotHot *slot);

// Locate a query's memoized slot by (kind, key). The slot state is split
// into parallel hot/cold SoA columns at the same row — db_locate_slot
// returns the hot record (everything db_revalidate / db_query_begin
// read), db_locate_slot_cold the lifecycle bookkeeping (computed_rev,
// changed_rev, last_fingerprint — only db_query_succeed/_fail need it).
// Both return NULL for an unwired kind / unclassified def / bad id.
QuerySlotHot*  db_locate_slot(struct db *s, QueryKind kind, uint64_t key);
QuerySlotCold* db_locate_slot_cold(struct db *s, QueryKind kind, uint64_t key);

#endif // ORE_DB_QUERY_INVALIDATE_H
