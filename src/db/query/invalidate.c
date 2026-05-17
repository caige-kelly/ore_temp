#include "invalidate.h"
#include "../db.h"
#include "../request/request.h"

QuerySlot *db_locate_slot(struct db *s, QueryKind kind, const void *key) {
  if (!key)
    return NULL;
  switch (kind) {
  case QUERY_TYPE_OF_DECL:
    return (QuerySlot *)vec_get(&s->defs.slots_type, ((DefId *)key)->idx);
  case QUERY_FN_SIGNATURE:
    return (QuerySlot *)vec_get(&s->defs.slots_signature, ((DefId *)key)->idx);

  case QUERY_CONST_EVAL:
    return (QuerySlot *)vec_get(&s->defs.slots_const_eval, ((DefId *)key)->idx);
  case QUERY_RESOLVE_REF:
    return (QuerySlot *)vec_get(&s->scopes.slots_resolve_ref,
                                ((ScopeId *)key)->idx);

  // The parse query is per-file: its slot lives in db.files.slots_ast
  // indexed by FileId. Slot pointers are NOT cached by callers — the
  // kind/key-centric query API re-resolves on every call, so column
  // reallocs are safe.
  case QUERY_FILE_AST: {
    uint32_t local = file_id_local(*(const FileId *)key);
    if (local >= s->files.slots_ast.count)
      return NULL;
    return (QuerySlot *)vec_get(&s->files.slots_ast, local);
  }

  // Module-scoped derived queries live in SoA columns on the thin
  // db.modules aggregate, indexed by ModuleId.
  case QUERY_TOP_LEVEL_INDEX:
  case QUERY_MODULE_EXPORTS: {
    ModuleId mid = *(const ModuleId *)key;
    if (mid.idx >= s->modules.slots_index.count)
      return NULL;
    return kind == QUERY_TOP_LEVEL_INDEX
               ? (QuerySlot *)vec_get(&s->modules.slots_index, mid.idx)
               : (QuerySlot *)vec_get(&s->modules.slots_exports, mid.idx);
  }

  // Sparse-keyed via HashMap. Key is a StrId pointer (the interned
  // dotted-path). Embedded ResolvePathEntry lives in db.arena —
  // pointer-stable for db lifetime.
  case QUERY_RESOLVE_PATH: {
    StrId path = *(const StrId *)key;
    ResolvePathEntry *entry =
        (ResolvePathEntry *)hashmap_get(&s->resolve_path, (uint64_t)path.idx);
    return entry ? &entry->slot : NULL;
  }

  default:
    return NULL;
  }
}

static RevalidateResult db_revalidate_impl(struct db *s, QuerySlot *slot);

// Cycle-safe wrapper. The verification walk recurses through deps; a
// dependency-graph cycle (mutually-recursive queries — sema's type ↔
// signature ↔ const-eval) would otherwise recurse unboundedly because
// verified_rev is only stamped AFTER the deps loop, so an in-progress
// slot offers no short-circuit. Mark the slot for the duration of its
// descent: re-entry means a cycle, which cannot be proven unchanged →
// RECOMPUTE (the compute path's QUERY_RUNNING→QUERY_BEGIN_CYCLE then
// resolves the actual cycle). Cleanup is centralized here so no early
// return in the impl can leak the mark.
RevalidateResult db_revalidate(struct db *s, QuerySlot *slot) {
  if (!slot)
    return DB_REVALIDATE_RECOMPUTE;
  if (slot->revalidating)
    return DB_REVALIDATE_RECOMPUTE;

  slot->revalidating = true;
  RevalidateResult r = db_revalidate_impl(s, slot);
  slot->revalidating = false;
  return r;
}

static RevalidateResult db_revalidate_impl(struct db *s, QuerySlot *slot) {
  if (slot->state != QUERY_DONE && slot->state != QUERY_ERROR)
    return DB_REVALIDATE_RECOMPUTE;

  uint64_t eff = db_effective_revision(s);
  if (slot->verified_rev == eff)
    return DB_REVALIDATE_SKIP_RECOMPUTE;

  // Durability fast-path (additive — purely an optimization). If no
  // input at this slot's durability tier has changed since it was last
  // verified, it provably cannot have changed: walk-free skip. Sound
  // because slot->durability is the MIN durability over all (transitive)
  // inputs and db_input_changed bumps dur_last_changed[i] for every
  // i <= the edited input's durability. If this doesn't fire we fall
  // through to the exact dep-fingerprint walk — identical to before.
  // Untracked-read slots opt out (their inputs aren't modeled as deps).
  if (!slot->has_untracked_read && slot->durability < DUR_COUNT &&
      atomic_load(&s->dur_last_changed[slot->durability]) <=
          slot->verified_rev) {
    slot->verified_rev = eff;
    return DB_REVALIDATE_SKIP_RECOMPUTE;
  }

  if (slot->has_untracked_read) {
#ifdef ORE_DEBUG_QUERIES
    s->query_stats[(int)slot->kind].recompute_due_to_untracked++;
#endif
    return DB_REVALIDATE_RECOMPUTE;
  }

  if (slot->deps) {
    for (size_t i = 0; i < slot->deps->count; i++) {
      QueryDep *dep = (QueryDep *)vec_get(slot->deps, i);
      if (!dep)
        continue;

      QuerySlot *dep_slot = db_locate_slot(s, dep->kind, dep->key);
      if (!dep_slot)
        return DB_REVALIDATE_RECOMPUTE;

      RevalidateResult dep_result = db_revalidate(s, dep_slot);
      if (dep_result == DB_REVALIDATE_RECOMPUTE)
        return DB_REVALIDATE_RECOMPUTE;

      if (dep_slot->state != QUERY_DONE)
        return DB_REVALIDATE_RECOMPUTE;
      if (dep_slot->fingerprint != dep->dep_fp)
        return DB_REVALIDATE_RECOMPUTE;
    }
  }

  slot->verified_rev = eff;
  return DB_REVALIDATE_SKIP_RECOMPUTE;
}
