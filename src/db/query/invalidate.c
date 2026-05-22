#include "invalidate.h"
#include "../db.h"
#include "../request/request.h"

// Route (kind, key) to the owning hot + cold slot column pair and the
// row index within them. Returns false — the slot does not exist — for
// an unwired kind, an unclassified / wrong-kind def, or an out-of-range
// id. Single-sourced so db_locate_slot and db_locate_slot_cold share
// one ~per-kind switch; each just projects out the column it wants.
static bool db_route_slot(struct db *s, QueryKind kind, uint64_t key,
                          Vec **out_hot, Vec **out_cold, uint32_t *out_row) {
  if (!key)
    return false;
  switch (kind) {
  // Def-keyed queries route through the per-kind tables: kinds[d]
  // selects the table, kind_row[d] the row (db_def_kind is in db.h).
  case QUERY_TYPE_OF_DECL: {
    DefId d = {.idx = (uint32_t)key};
    *out_row = *(uint32_t *)vec_get(&s->defs.kind_row, d.idx);
    switch (db_def_kind(s, d)) {
    case KIND_FUNCTION:
      *out_hot = &s->fns.slot_type_hot;
      *out_cold = &s->fns.slot_type_cold;
      return true;
    case KIND_STRUCT:
      *out_hot = &s->structs.slot_type_hot;
      *out_cold = &s->structs.slot_type_cold;
      return true;
    case KIND_UNION:
      *out_hot = &s->unions.slot_type_hot;
      *out_cold = &s->unions.slot_type_cold;
      return true;
    case KIND_ENUM:
      *out_hot = &s->enums.slot_type_hot;
      *out_cold = &s->enums.slot_type_cold;
      return true;
    case KIND_EFFECT:
      *out_hot = &s->effects.slot_type_hot;
      *out_cold = &s->effects.slot_type_cold;
      return true;
    case KIND_HANDLER:
      *out_hot = &s->handlers.slot_type_hot;
      *out_cold = &s->handlers.slot_type_cold;
      return true;
    case KIND_VARIABLE:
      *out_hot = &s->variables.slot_type_hot;
      *out_cold = &s->variables.slot_type_cold;
      return true;
    case KIND_CONSTANT:
      *out_hot = &s->constants.slot_type_hot;
      *out_cold = &s->constants.slot_type_cold;
      return true;
    default:
      return false;
    }
  }
  case QUERY_FN_SIGNATURE: {
    DefId d = {.idx = (uint32_t)key};
    if (db_def_kind(s, d) != KIND_FUNCTION)
      return false;
    *out_row = *(uint32_t *)vec_get(&s->defs.kind_row, d.idx);
    *out_hot = &s->fns.slot_signature_hot;
    *out_cold = &s->fns.slot_signature_cold;
    return true;
  }
  case QUERY_INFER_BODY: {
    DefId d = {.idx = (uint32_t)key};
    if (db_def_kind(s, d) != KIND_FUNCTION)
      return false;
    *out_row = *(uint32_t *)vec_get(&s->defs.kind_row, d.idx);
    *out_hot = &s->fns.slot_infer_hot;
    *out_cold = &s->fns.slot_infer_cold;
    return true;
  }
  case QUERY_BODY_SCOPES: {
    DefId d = {.idx = (uint32_t)key};
    if (db_def_kind(s, d) != KIND_FUNCTION)
      return false;
    *out_row = *(uint32_t *)vec_get(&s->defs.kind_row, d.idx);
    *out_hot = &s->fns.slot_body_scopes_hot;
    *out_cold = &s->fns.slot_body_scopes_cold;
    return true;
  }
  case QUERY_CONST_EVAL: {
    DefId d = {.idx = (uint32_t)key};
    if (db_def_kind(s, d) != KIND_CONSTANT)
      return false;
    *out_row = *(uint32_t *)vec_get(&s->defs.kind_row, d.idx);
    *out_hot = &s->constants.slot_const_eval_hot;
    *out_cold = &s->constants.slot_const_eval_cold;
    return true;
  }
  // Per-(scope, name) name resolution. db.resolve_ref_cache routes the
  // packed (scope<<32 | name) key to a row in db.resolve_ref.
  case QUERY_RESOLVE_REF: {
    void *rowp = hashmap_get(&s->resolve_ref_cache, key);
    if (!rowp)
      return false;
    *out_row = (uint32_t)(uintptr_t)rowp;
    *out_hot = &s->resolve_ref.slots_hot;
    *out_cold = &s->resolve_ref.slots_cold;
    return true;
  }
  // The parse query is per-file: its slot lives in db.files.slots_ast_*
  // indexed by FileId. Slot pointers are NOT cached by callers — the
  // kind/key-centric query API re-resolves on every call, so column
  // reallocs are safe.
  case QUERY_FILE_AST: {
    uint32_t local = file_id_local((FileId){.idx = (uint32_t)key});
    if (local >= s->files.slots_ast_hot.count)
      return false;
    *out_row = local;
    *out_hot = &s->files.slots_ast_hot;
    *out_cold = &s->files.slots_ast_cold;
    return true;
  }
  // Per-file node→DefId reverse-index query. Same FileId keying as the
  // parse query; its slot column is files.slots_node_to_def_*.
  case QUERY_NODE_TO_DECL: {
    uint32_t local = file_id_local((FileId){.idx = (uint32_t)key});
    if (local >= s->files.slots_node_to_def_hot.count)
      return false;
    *out_row = local;
    *out_hot = &s->files.slots_node_to_def_hot;
    *out_cold = &s->files.slots_node_to_def_cold;
    return true;
  }
  // Module-scoped derived queries live in SoA columns on the thin
  // db.modules aggregate, indexed by ModuleId.
  case QUERY_TOP_LEVEL_INDEX:
  case QUERY_MODULE_EXPORTS: {
    ModuleId mid = {.idx = (uint32_t)key};
    if (mid.idx >= s->modules.slots_index_hot.count)
      return false;
    *out_row = mid.idx;
    if (kind == QUERY_TOP_LEVEL_INDEX) {
      *out_hot = &s->modules.slots_index_hot;
      *out_cold = &s->modules.slots_index_cold;
    } else {
      *out_hot = &s->modules.slots_exports_hot;
      *out_cold = &s->modules.slots_exports_cold;
    }
    return true;
  }
  // (ModuleId, AstId)-keyed stable DefId materialization.
  // db.def_by_identity routes the packed (mid<<32 | ast_id) key to a row
  // in db.def_identity, so the DefId persists across module_exports
  // re-runs (the row is allocated once and never moves).
  case QUERY_DEF_IDENTITY: {
    void *rowp = hashmap_get(&s->def_by_identity, key);
    if (!rowp)
      return false;
    *out_row = (uint32_t)(uintptr_t)rowp;
    *out_hot = &s->def_identity.slots_hot;
    *out_cold = &s->def_identity.slots_cold;
    return true;
  }
  // Per-(interned dotted-path) resolution. db.resolve_path_cache routes
  // the StrId key to a row in db.resolve_path.
  case QUERY_RESOLVE_PATH: {
    void *rowp = hashmap_get(&s->resolve_path_cache, key);
    if (!rowp)
      return false;
    *out_row = (uint32_t)(uintptr_t)rowp;
    *out_hot = &s->resolve_path.slots_hot;
    *out_cold = &s->resolve_path.slots_cold;
    return true;
  }
  default:
    return false;
  }
}

QuerySlotHot *db_locate_slot(struct db *s, QueryKind kind, uint64_t key) {
  Vec *hot, *cold;
  uint32_t row;
  if (!db_route_slot(s, kind, key, &hot, &cold, &row))
    return NULL;
  return (QuerySlotHot *)vec_get(hot, row);
}

QuerySlotCold *db_locate_slot_cold(struct db *s, QueryKind kind, uint64_t key) {
  Vec *hot, *cold;
  uint32_t row;
  if (!db_route_slot(s, kind, key, &hot, &cold, &row))
    return NULL;
  return (QuerySlotCold *)vec_get(cold, row);
}

// The dep-free part of revalidation. Returns true (with *out set) if the
// slot resolves without walking deps; false if the dep walk is needed.
static bool revalidate_precheck(struct db *s, QuerySlotHot *slot,
                                uint64_t eff, RevalidateResult *out) {
  if (slot->state != QUERY_DONE && slot->state != QUERY_ERROR) {
    *out = DB_REVALIDATE_RECOMPUTE;
    return true;
  }
  if (slot->verified_rev == eff) {
    *out = DB_REVALIDATE_SKIP_RECOMPUTE;
    return true;
  }
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
    *out = DB_REVALIDATE_SKIP_RECOMPUTE;
    return true;
  }
  if (slot->has_untracked_read) {
#ifdef ORE_DEBUG_QUERIES
    s->query_stats[(int)slot->kind].recompute_due_to_untracked++;
#endif
    *out = DB_REVALIDATE_RECOMPUTE;
    return true;
  }
  return false; // dep walk needed
}

// One node of the explicit DFS worklist (db.revalidate_stack).
typedef struct {
  QuerySlotHot *slot;
  uint32_t      dep_i;    // next dep index to process
  bool          started;  // precheck has run; in the dep loop
  bool          awaiting; // a child frame was pushed for deps[dep_i]
} RevalFrame;

// Verify a memoized slot against the current revision — SKIP_RECOMPUTE
// if it (and its transitive deps) are provably unchanged, RECOMPUTE
// otherwise.
//
// An explicit DFS worklist, not native recursion: C-stack use is O(1)
// in dependency-graph depth. The `revalidating` flag marks every slot
// currently on the worklist; a dep that is already marked is an
// in-progress ancestor — a dependency-graph cycle (sema's mutually-
// recursive type ↔ signature ↔ const-eval). A cycle cannot be proven
// unchanged, so it resolves to RECOMPUTE, exactly as the former
// recursive db_revalidate did on re-entry. Every frame clears its slot's
// mark when it pops, on every exit path, so no mark leaks.
//
// db.revalidate_stack is reused across calls and never re-entered:
// db_revalidate only reads slot state — it never runs a query body.
RevalidateResult db_revalidate(struct db *s, QuerySlotHot *root) {
  if (!root)
    return DB_REVALIDATE_RECOMPUTE;
  if (root->revalidating)
    return DB_REVALIDATE_RECOMPUTE;

  uint64_t eff = db_effective_revision(s);

  Vec *stk = &s->revalidate_stack;
  if (stk->element_size == 0)
    vec_init(stk, sizeof(RevalFrame));
  vec_clear(stk);

  RevalidateResult result = DB_REVALIDATE_SKIP_RECOMPUTE; // last frame popped
  RevalFrame f0 = {.slot = root, .dep_i = 0, .started = false,
                   .awaiting = false};
  vec_push(stk, &f0);
  root->revalidating = true;

  while (stk->count > 0) {
    RevalFrame *f = (RevalFrame *)vec_get(stk, stk->count - 1);
    QuerySlotHot *slot = f->slot;

    // (a) First visit — the dep-free precheck.
    if (!f->started) {
      f->started = true;
      RevalidateResult pr;
      if (revalidate_precheck(s, slot, eff, &pr)) {
        slot->revalidating = false;
        result = pr;
        stk->count--;
        continue;
      }
    }

    Vec *deps = slot->deps;
    size_t ndeps = deps ? deps->count : 0;

    // (b) Resuming after a child completed — apply its result to the
    //     dep we were awaiting (`result` holds that child's outcome).
    if (f->awaiting) {
      f->awaiting = false;
      QueryDep *dep = (QueryDep *)vec_get(deps, f->dep_i);
      QuerySlotHot *dep_slot = db_locate_slot(s, dep->kind, dep->key);
      if (result == DB_REVALIDATE_RECOMPUTE || !dep_slot ||
          dep_slot->state != QUERY_DONE ||
          dep_slot->fingerprint != dep->dep_fp) {
        slot->revalidating = false;
        result = DB_REVALIDATE_RECOMPUTE;
        stk->count--;
        continue;
      }
      f->dep_i++; // this dep verified clean
    }

    // (c) Walk deps until one needs descending into, or all pass.
    bool descended = false;
    while (f->dep_i < ndeps) {
      QueryDep *dep = (QueryDep *)vec_get(deps, f->dep_i);
      QuerySlotHot *dep_slot = db_locate_slot(s, dep->kind, dep->key);
      if (!dep_slot || dep_slot->revalidating) {
        // Missing slot, or an in-progress ancestor (cycle): cannot be
        // proven unchanged → this slot RECOMPUTEs.
        slot->revalidating = false;
        result = DB_REVALIDATE_RECOMPUTE;
        stk->count--;
        descended = true; // (frame is done — skip the all-pass tail)
        break;
      }
      // Descend into dep_slot. `f` may dangle after vec_push (realloc)
      // — it is not touched again this iteration.
      f->awaiting = true;
      dep_slot->revalidating = true;
      RevalFrame cf = {.slot = dep_slot, .dep_i = 0, .started = false,
                       .awaiting = false};
      vec_push(stk, &cf);
      descended = true;
      break;
    }
    if (descended)
      continue;

    // (d) All deps verified clean.
    slot->verified_rev = eff;
    slot->revalidating = false;
    result = DB_REVALIDATE_SKIP_RECOMPUTE;
    stk->count--;
  }

  return result;
}
