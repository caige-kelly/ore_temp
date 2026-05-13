#ifndef ORE_SEMA_QUERY_ENGINE_H
#define ORE_SEMA_QUERY_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "query.h"
#include "../storage/hashmap.h"

// === Fingerprints ===
//
// A query result's fingerprint is a hash of the result's externally
// observable content. When an input changes and a query reruns, if the
// new fingerprint matches the old, downstream queries can skip rerun
// (they read the same value either way). This is the key invariant
// behind Salsa-style "early cutoff" — we trade a hash compute for
// avoiding cascading recomputation.

typedef struct {
    HashMap memo_table;
  
    Vec query_stack;
  } QueryEngine;

Fingerprint query_fingerprint_from_pointer(const void *ptr);
Fingerprint query_fingerprint_from_bytes(const void *data, size_t size);
Fingerprint query_fingerprint_from_u64(uint64_t v);
Fingerprint query_fingerprint_combine(Fingerprint a, Fingerprint b);

// Stamp a freshly-computed fingerprint onto a slot. Call after the
// query body finishes computing its result, before sema_query_succeed.
// Idempotent; safe to call multiple times before succeed.
void query_slot_set_fingerprint(struct QuerySlot *slot, Fingerprint fp);

// === Dependency iteration ===
//
// During a slot's compute, every child query call records itself onto
// the parent's frame deps (see record_dep_on_parent in query.c). When
// the parent succeeds, those deps are stamped onto the slot. The
// helpers below let future incremental code walk the recorded deps to
// check if any have changed since this slot was last verified.

typedef bool (*QueryDepVisitor)(struct QueryDep dep, void *user_data);

// Iterate every dep recorded by `slot`. The visitor returns true to
// continue, false to stop early. Order matches the order in which
// dependencies were called during compute.
void query_slot_visit_deps(struct QuerySlot *slot, QueryDepVisitor visit,
                           void *user_data);

// Number of deps recorded by `slot`. Returns 0 for slots that haven't
// computed or computed with no deps.
size_t query_slot_dep_count(struct QuerySlot *slot);

#endif // ORE_SEMA_QUERY_ENGINE_H
