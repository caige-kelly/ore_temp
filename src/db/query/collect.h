#ifndef ORE_DB_QUERY_COLLECT_H
#define ORE_DB_QUERY_COLLECT_H

#include "query.h"

/*
    Visit every QuerySlot owned by a db, regardless of slot state. Iteration
    order is implementation-defined — slots are spread across SoA columns
    (defs/scopes), one HashMap (resolve_path), and per-ModuleInfo embedded
    fields, and we walk them in that order. Callers that need stable
    ordering must collect into their own structure and sort by their key.

    Consumers:
      - Diagnostic collection: walk every slot's diags accumulator on each
        LSP publish / CLI render. The slot pointer's kind field identifies
        the query kind for routing.
      - Future eviction walker: identify cold slots (compare
        slot->last_accessed_rev against current_revision) and reclaim.

    The visitor signature:
      - `slot`  — non-null pointer to a real QuerySlot. May be in any
                  state (QUERY_EMPTY through QUERY_ERROR). The pointer is
                  valid for the duration of the visitor call only — Vecs
                  and HashMaps may relocate buffers between calls.
      - `kind`  — the QueryKind associated with this slot's column / home.
                  Not necessarily equal to `slot->kind` (an EMPTY slot has
                  kind=0); use this parameter, not the slot field.
      - `key`   — a pointer to the key for this slot, in a form usable
                  with db_locate_slot. Lifetime is the visitor call only;
                  do not store. For SoA columns it points to a stack temp,
                  for HashMap-embedded slots it's a heap pointer.
      - `user_data` — caller-supplied context.

    The visitor must not insert into or grow any slot-bearing container
    during iteration — doing so can invalidate the in-flight cursor.
*/

typedef void (*DbSlotVisitor)(QuerySlot *slot, QueryKind kind,
                              const void *key, void *user_data);

void db_for_each_slot(struct db *s, DbSlotVisitor visit, void *user_data);

#endif // ORE_DB_QUERY_COLLECT_H
