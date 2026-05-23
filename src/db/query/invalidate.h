#ifndef ORE_DB_QUERY_INVALIDATE_H
#define ORE_DB_QUERY_INVALIDATE_H

#include <stdbool.h>
#include "query.h"

struct db;

// db_verify — is this slot's memoized value still valid at the current
// revision? Pulls each recorded dep through the typed dispatch table
// (recursively verifying-or-recomputing) and compares the dep's now-
// current fingerprint against what we recorded. Returns true if every
// dep is unchanged (the slot's cached value can be reused), false
// otherwise (the body must rerun).
//
// Called from db_query_begin AFTER it has pushed the slot's frame onto
// query_stack: verify-driven dep pulls then record idempotently onto
// the slot's own frame (dedup against existing deps), so dep recording
// stays correct without any sentinel-frame trick.
//
// Cycle detection rides on QUERY_RUNNING: db_query_begin sets RUNNING
// when pushing for verify, so a recursive pull of the same slot hits
// the existing QUERY_BEGIN_CYCLE path in db_query_begin.
bool db_verify(struct db *s, QuerySlotHot *slot);

// Locate a query's memoized slot by (kind, key). The slot state is split
// into parallel hot/cold SoA columns at the same row — db_locate_slot
// returns the hot record (read on every dep walk), db_locate_slot_cold
// the lifecycle bookkeeping (computed_rev — used by tests). Both return
// NULL for an unwired kind / unclassified def / bad id.
QuerySlotHot*  db_locate_slot(struct db *s, QueryKind kind, uint64_t key);
QuerySlotCold* db_locate_slot_cold(struct db *s, QueryKind kind, uint64_t key);

#endif // ORE_DB_QUERY_INVALIDATE_H
