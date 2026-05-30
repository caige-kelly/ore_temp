// Engine verify — dep walk for cached slot re-validation.
//
// Called from db_query_begin for slots in DONE/ERROR state. Walks the
// slot's recorded deps; for each, pulls the dep through dispatch (which
// handles cache-vs-recompute internally), then compares the dep's
// current fingerprint to the recorded dep_fp. Any mismatch → the slot's
// memoized value is stale → caller recomputes.
//
// Durability fast-path: if no input at the slot's tier has changed since
// it was last verified, the slot's value is provably unchanged — skip
// the dep walk entirely.

#define ORE_ENGINE_PRIVATE
#include "engine.h"
#include "engine_internal.h"

#include "../db.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

bool db_engine_verify(db_query_ctx *ctx, QuerySlotHot *slot) {
  if (!slot)
    return false;

  uint64_t eff = db_effective_revision(ctx);

  // Trivially current — already verified at this revision.
  if (slot->verified_rev == eff)
    return true;

  struct db *s = (struct db *)ctx;

  // Durability fast-path — a SKIP-OPTIMIZATION only, never the source
  // of correctness (that's the per-dep fingerprint comparison below).
  // If no input at our tier has changed since we last verified, the
  // value is provably unchanged, so skip the dep walk. slot->durability
  // is the cached MIN-at-succeed; per-dep dep_dur is recorded but not
  // yet used here (Phase 8 upgrade: compute MIN over LIVE deps at
  // verify time for sharper bounds when stale deps would have shifted
  // MIN).
  if (slot->durability < DUR_COUNT &&
      atomic_load(&s->dur_last_changed[slot->durability]) <=
          slot->verified_rev) {
    slot->verified_rev = eff;
    return true;
  }

  // Dep walk. For each dep, pull via dispatch (the wrapper handles
  // cache-vs-recompute); then compare the dep's current fp to what
  // we recorded. The FINGERPRINT is the authoritative comparand — we
  // never inspect the dep's verified_rev. This is what makes INPUT
  // deps work uniformly: an INPUT kind (e.g. SOURCE_TEXT) has a no-op
  // recompute thunk, and its slot fingerprint is maintained directly
  // by db_input_set, so the comparison below sees the live source hash
  // and recomputes us iff it moved — per-input precise, with no
  // special-case in this loop.
  //
  // POST-A2: slot columns are PagedVec-backed, so slot pointers
  // obtained BEFORE the pull stay valid AFTER it. No re-locate dance
  // needed — we resolve dep_slot once, then read its fp.
  Vec *deps = slot->deps;
  size_t ndeps = deps ? deps->count : 0;
  for (size_t i = 0; i < ndeps; i++) {
    QueryDep *dep = (QueryDep *)vec_get(deps, i);
    QueryKind dep_kind = dep->kind;
    uint64_t dep_key = dep->key;
    Fingerprint recorded_fp = dep->dep_fp;

    // Resolve BEFORE the pull; pointer stays valid across pull
    // thanks to PagedVec storage.
    QuerySlotHot *dep_slot = db_engine_locate_slot(ctx, dep_kind, dep_key);
    RecomputeFn pull = db_engine_recompute_dispatch[dep_kind];
    if (pull)
      pull(ctx, dep_key);

    if (!dep_slot || dep_slot->state != QUERY_DONE ||
        dep_slot->fingerprint != recorded_fp) {
      return false;
    }
  }

  slot->verified_rev = eff;
  return true;
}
