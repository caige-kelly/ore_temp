#ifndef ORE_DB_QUERY_COLLECT_H
#define ORE_DB_QUERY_COLLECT_H

#include "query.h"

/*
    Visit every QuerySlot owned by a db, regardless of slot state. Slots
    live in the per-kind table slot columns (db.fns / db.structs / …), the
    per-file slots_ast column, the per-module slot columns, and three
    HashMap caches (resolve_path, def_by_identity, resolve_ref). Iteration
    order is implementation-defined.

    Consumer: db_free's teardown walk, which releases each slot's
    malloc-owned deps buffer. (A future eviction walker could also use
    this to find cold slots via slot->last_accessed_rev. Diagnostics no
    longer use this walk — they live in db.diags, keyed independently.)

    The visitor signature:
      - `slot`  — non-null pointer to a real QuerySlot. May be in any
                  state. Valid for the visitor call only — Vecs and
                  HashMaps may relocate buffers between calls.
      - `kind`  — the QueryKind associated with this slot's column / home.
      - `key`   — pointer to the slot's key (HashMap-embedded slots) or
                  NULL (per-kind table slots — the teardown visitor
                  ignores it). Lifetime is the visitor call only.
      - `user_data` — caller-supplied context.

    The visitor must not insert into or grow any slot-bearing container
    during iteration.
*/

typedef void (*DbSlotVisitor)(QuerySlot *slot, QueryKind kind,
                              const void *key, void *user_data);

void db_for_each_slot(struct db *s, DbSlotVisitor visit, void *user_data);

#endif // ORE_DB_QUERY_COLLECT_H
