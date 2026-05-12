#ifndef ORE_SEMA_QUERY_COLLECT_H
#define ORE_SEMA_QUERY_COLLECT_H

#include "query.h"

// Visit every QuerySlot owned by a Sema, regardless of slot state. The
// iteration order is implementation-defined (slots are spread across
// many HashMaps + side tables) — callers that need a stable order
// must sort by their own key after collection.
//
// Used by the diagnostic collector in diag.c (walks every slot's
// `diags` accumulator on each LSP publish / CLI render). When the
// eviction walker lands, it'll also walk via this function to find
// evictable slots and call arena_free on their diag_arena.
typedef void (*SemaSlotVisitor)(struct QuerySlot *slot, void *user_data);
void sema_for_each_slot(struct Sema *s, SemaSlotVisitor visit, void *user_data);

#endif // ORE_SEMA_QUERY_COLLECT_H
