#ifndef ORE_SEMA_QUERY_ENGINE_H
#define ORE_SEMA_QUERY_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "query.h"

// Higher-level query infrastructure.
//
// query.h owns the slot lifecycle (begin/succeed/fail) and the cycle
// detection. query_engine.h sits on top: fingerprint computation,
// revision tracking, dep iteration. None of these are wired into the
// invalidation loop yet — they're the substrate the future incremental
// layer plugs into.
//
// The split keeps query.h dependency-free of any policy and makes
// query_engine.h the place to look for "what changed in this build vs
// last build" plumbing when we eventually need it.

struct Sema;

// === Fingerprints ===
//
// A query result's fingerprint is a hash of the result's externally
// observable content. When an input changes and a query reruns, if the
// new fingerprint matches the old, downstream queries can skip rerun
// (they read the same value either way). This is the key invariant
// behind Salsa-style "early cutoff" — we trade a hash compute for
// avoiding cascading recomputation.
//
// Today no one reads the fingerprint, but the hash functions and slot
// field exist so the cutoff path is mechanical to add.

Fingerprint query_fingerprint_from_pointer(const void *ptr);
Fingerprint query_fingerprint_from_bytes(const void *data, size_t size);
Fingerprint query_fingerprint_from_u64(uint64_t v);
Fingerprint query_fingerprint_combine(Fingerprint a, Fingerprint b);

// Stamp a freshly-computed fingerprint onto a slot. Call after the
// query body finishes computing its result, before sema_query_succeed.
// Idempotent; safe to call multiple times before succeed.
void query_slot_set_fingerprint(struct QuerySlot *slot, Fingerprint fp);

// === Revisions ===
//
// Bumped when an input fact changes. Today no input is mutable so
// nothing calls bump; the function exists to lock in the API and let
// future code (incremental file edits, LSP edits) bump cleanly.

void sema_bump_revision(struct Sema *s);
uint64_t sema_current_revision(struct Sema *s);

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

// === LRU eviction (Layer 7.7) ===
//
// The engine tracks slot creation count via Sema.slot_count and
// per-slot last_accessed_rev (touched on every sema_query_begin).
// When the cache exceeds Sema.slot_budget, callers can request
// eviction via sema_evict_lru.
//
// First-cut implementation: a stub that updates slot_count toward
// the target without actually evicting. Real eviction needs a
// "slot registry" — a master list of every QuerySlot in the
// system — so the walker can iterate without blowing through
// every per-cache HashMap. That registry is the one piece of
// infrastructure we punt on; the API below is the seam.
//
// `sema_set_slot_budget` defaults to 50,000 if the caller never
// sets one. Long-running LSP processes should pick a value
// roughly proportional to the project size.

void sema_set_slot_budget(struct Sema *s, size_t budget);
size_t sema_slot_count(struct Sema *s);

// Evict slots until total count ≤ target. Stub today — does
// nothing — but the API exists so calls from the LSP shell are
// shape-stable when the registry lands.
void sema_evict_lru(struct Sema *s, size_t target);

#endif // ORE_SEMA_QUERY_ENGINE_H
