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
  // db.modules aggregate, indexed by NamespaceId.
  case QUERY_TOP_LEVEL_INDEX:
  case QUERY_NAMESPACE_SCOPES:
  case QUERY_NAMESPACE_TYPE:
  case QUERY_UNUSED_WARNINGS: {
    NamespaceId nsid = {.idx = (uint32_t)key};
    if (nsid.idx >= s->namespaces.slots_index_hot.count)
      return false;
    *out_row = nsid.idx;
    if (kind == QUERY_TOP_LEVEL_INDEX) {
      *out_hot = &s->namespaces.slots_index_hot;
      *out_cold = &s->namespaces.slots_index_cold;
    } else if (kind == QUERY_NAMESPACE_SCOPES) {
      *out_hot = &s->namespaces.slots_exports_hot;
      *out_cold = &s->namespaces.slots_exports_cold;
    } else if (kind == QUERY_NAMESPACE_TYPE) {
      *out_hot = &s->namespaces.slots_namespace_type_hot;
      *out_cold = &s->namespaces.slots_namespace_type_cold;
    } else {
      *out_hot = &s->namespaces.slots_unused_warnings_hot;
      *out_cold = &s->namespaces.slots_unused_warnings_cold;
    }
    return true;
  }
  // (NamespaceId, AstId)-keyed stable DefId materialization.
  // db.def_by_identity routes the packed (nsid<<32 | ast_id) key to a row
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
  // Per-decl AST handle. db.decl_ast_cache routes the packed
  // (file_local<<32 | ast_id) key to a row in db.decl_ast.
  case QUERY_DECL_AST: {
    void *rowp = hashmap_get(&s->decl_ast_cache, key);
    if (!rowp)
      return false;
    *out_row = (uint32_t)(uintptr_t)rowp;
    *out_hot = &s->decl_ast.slots_hot;
    *out_cold = &s->decl_ast.slots_cold;
    return true;
  }
  // Per-file @import refs. Same FileId-keyed shape as QUERY_FILE_AST;
  // its slot column is files.slots_file_imports_*.
  case QUERY_FILE_IMPORTS: {
    uint32_t local = file_id_local((FileId){.idx = (uint32_t)key});
    if (local >= s->files.slots_file_imports_hot.count)
      return false;
    *out_row = local;
    *out_hot = &s->files.slots_file_imports_hot;
    *out_cold = &s->files.slots_file_imports_cold;
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

// db_verify — is this slot's memoized value still valid at the current
// effective revision?
//
// Pull-based: for each recorded dep, invoke the dep's typed wrapper via
// the dispatch table. The wrapper's own db_query_begin handles
// cache-vs-recompute (recursively); after the call, the dep slot's
// fingerprint reflects its current value. Compare that against the
// fingerprint we recorded when this slot last ran — if any dep's value
// changed, this slot's body must rerun; if all match, the memoized
// value is still correct (value-based early-cutoff).
//
// Cycle detection rides on QUERY_RUNNING — db_query_begin sets the
// slot to RUNNING when it pushes its frame for verify, so a recursive
// pull of the same slot returns QUERY_BEGIN_CYCLE through the wrapper's
// existing cycle handler. The dep slot's value is unchanged in that
// case, so the fingerprint comparison below decides naturally.
bool db_verify(struct db *s, QuerySlotHot *slot) {
  if (!slot)
    return false;

  uint64_t eff = db_effective_revision(s);

  // Trivially current — verified at this exact revision.
  if (slot->verified_rev == eff)
    return true;

  // Untracked-read slots can't prove cleanliness via recorded deps
  // (their inputs aren't modeled). Conservative: always rerun.
  if (slot->has_untracked_read) {
#ifdef ORE_DEBUG_QUERIES
    s->query_stats[(int)slot->kind].recompute_due_to_untracked++;
#endif
    return false;
  }

  // Durability fast-path. slot->durability is the MIN durability over
  // (transitive) inputs; db_input_changed bumps dur_last_changed[i]
  // for every i <= the edited input's durability. If no input at this
  // slot's tier has changed since we last verified, the slot's value
  // provably hasn't changed: walk-free skip.
  if (slot->durability < DUR_COUNT &&
      atomic_load(&s->dur_last_changed[slot->durability]) <=
          slot->verified_rev) {
    slot->verified_rev = eff;
    return true;
  }

  // Dep walk. Pull each recorded dep through its typed wrapper; the
  // wrapper recursively verifies-or-recomputes. After the pull, the
  // dep slot's fingerprint is its current value — compare to what we
  // recorded last time this slot ran.
  Vec *deps = slot->deps;
  size_t ndeps = deps ? deps->count : 0;
  for (size_t i = 0; i < ndeps; i++) {
    QueryDep *dep = (QueryDep *)vec_get(deps, i);

    // Snapshot the recorded dep_fp BEFORE the pull. The pull's
    // wrapper unconditionally calls record_dep_on_parent against the
    // current top of query_stack (= this slot's frame); its dedup
    // refreshes the in-place dep entry's dep_fp to the dep's now-
    // current fingerprint. Reading dep->dep_fp after the pull would
    // see the refreshed value and the comparison would be trivial.
    QueryKind dep_kind = dep->kind;
    uint64_t dep_key = dep->key;
    Fingerprint recorded_fp = dep->dep_fp;

    RecomputeFn pull = s->recompute_dispatch[dep_kind];
    if (pull) {
      pull(s, dep_key);
    }
    // Re-locate the dep slot: column reallocs during the nested
    // wrapper call may have invalidated any prior pointer for this kind.
    QuerySlotHot *dep_slot = db_locate_slot(s, dep_kind, dep_key);
    if (!dep_slot || dep_slot->state != QUERY_DONE ||
        dep_slot->fingerprint != recorded_fp) {
      // Dep is missing / errored / value-changed — this slot is stale.
      return false;
    }
  }

  slot->verified_rev = eff;
  return true;
}
