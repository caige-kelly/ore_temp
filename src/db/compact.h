#ifndef ORE_DB_COMPACT_H
#define ORE_DB_COMPACT_H

#include "db.h"

// Mark-and-copy compaction for the four shared salsa pools. Each
// compactor walks the per-kind columns that own ranges into its pool,
// builds a sorted (old_off, len, new_off) remap table, allocates a
// fresh pool, copies live ranges into it, rewrites every column entry
// with the new offsets, then swaps in the new pool and frees the old.
//
// Triggered from db_request_end via db_pools_maybe_compact, which
// checks per-pool growth heuristics. See plan file for the
// architectural rationale and threshold tuning.

#define ORE_COMPACT_MIN_THRESHOLD   4096u
#define ORE_COMPACT_GROWTH_FACTOR   2u

void db_compact_node_types_pool(struct db *s);
void db_compact_body_scope_pools(struct db *s);
void db_compact_decl_pool(struct db *s);

#endif // ORE_DB_COMPACT_H
