#include "invalidate.h"

#include <stddef.h>

#include "../../common/vec.h"
#include "../modules/def_map.h"
#include "../modules/inputs.h"
#include "../modules/modules.h"
#include "../request/snapshot.h"
#include "../resolve/scope_index.h"
#include "../sema.h"

// Slot dispatch — maps (QueryKind, key) → QuerySlot*.
//
// Each query stores its slot somewhere kind-specific:
//   QUERY_MODULE_AST          → ((InputInfo*)key)->ast_query
//   QUERY_MODULE_DEF_MAP      → ((ModuleInfo*)key)->def_map_query
//   QUERY_MODULE_EXPORTS      → ((ModuleInfo*)key)->exports_query
//   QUERY_DEF_FOR_NAME        → ((DefMapEntry*)key)->query
//   QUERY_FN_SCOPE_INDEX      → ((ScopeIndexResult*)key)->query
//
// Other kinds (resolve_ref, type_of_decl, etc.) don't yet have
// centrally-addressable slots; we return NULL for those and treat
// them as "non-cacheable for invalidation purposes." The walker
// conservatively triggers recompute in that case.
struct QuerySlot *sema_locate_slot(struct Sema *s, QueryKind kind,
                                   const void *key) {
  (void)s;
  if (!key)
    return NULL;
  switch (kind) {
  case QUERY_MODULE_AST:
    return &((struct InputInfo *)key)->ast_query;
  case QUERY_MODULE_DEF_MAP:
    return &((struct ModuleInfo *)key)->def_map_query;
  case QUERY_MODULE_EXPORTS:
    return &((struct ModuleInfo *)key)->exports_query;
  case QUERY_DEF_FOR_NAME:
    return &((struct DefMapEntry *)key)->query;
  case QUERY_FN_SCOPE_INDEX:
    return &((struct ScopeIndexResult *)key)->query;
  case QUERY_TYPE_OF_DECL:
  case QUERY_LAYOUT_OF_TYPE:
  case QUERY_INSTANTIATE_DECL:
  case QUERY_EFFECT_SIG:
  case QUERY_BODY_EFFECTS:
  case QUERY_MODULE_FOR_PATH:
  case QUERY_TOP_LEVEL_INDEX:
  case QUERY_SCOPE_FOR_NODE:
  case QUERY_SCOPE_DECLS:
  case QUERY_SCOPE_PARENT:
  case QUERY_EFFECT_OPS_VISIBLE:
  case QUERY_RESOLVE_REF:
  case QUERY_RESOLVE_PATH:
  case QUERY_NODE_TO_DECL:
  case QUERY_CONST_EVAL:
    // No centrally-addressable slot today. Wire when un-prune
    // brings these queries online.
    return NULL;
  }
  return NULL;
}

RevalidateResult sema_revalidate(struct Sema *s, struct QuerySlot *slot) {
  if (!slot)
    return REVALIDATE_NOT_APPLICABLE;
  if (slot->state != QUERY_DONE)
    return REVALIDATE_NOT_APPLICABLE;

  // Already verified at this revision — fast path. Use the
  // effective revision (request_revision if pinned, else
  // current_revision) so requests under a snapshot see
  // consistent answers even when current_revision moves.
  uint64_t eff = sema_effective_revision(s);
  if (slot->verified_rev == eff)
    return REVALIDATE_SKIP_RECOMPUTE;

  // Walk recorded deps. For each, re-validate it (recursive), then
  // compare its current fingerprint to what we recorded. A mismatch
  // means our cached value is stale.
  if (slot->deps) {
    for (size_t i = 0; i < slot->deps->count; i++) {
      struct QueryDep *dep =
          (struct QueryDep *)vec_get(slot->deps, i);
      if (!dep)
        continue;

      struct QuerySlot *dep_slot =
          sema_locate_slot(s, dep->kind, dep->key);
      if (!dep_slot) {
        // Dep is non-addressable; conservatively recompute.
        // (Common when `slot` records deps on queries whose slot
        // dispatch hasn't been wired yet.)
        return REVALIDATE_RECOMPUTE;
      }

      // Recurse. If the dep itself is stale, refresh it before
      // comparing fingerprints.
      sema_revalidate(s, dep_slot);

      // After recursion: if the dep's slot has been recomputed
      // (state may have transitioned to EMPTY then back), or if
      // the dep simply has a different fingerprint than what we
      // recorded, our cached value is stale.
      if (dep_slot->state != QUERY_DONE)
        return REVALIDATE_RECOMPUTE;
      if (dep_slot->fingerprint != dep->dep_fp)
        return REVALIDATE_RECOMPUTE;
    }
  }

  // Every dep's fingerprint matches what we recorded. Verify our
  // slot at the effective revision so future revalidate calls
  // can fast-path.
  slot->verified_rev = eff;
  return REVALIDATE_SKIP_RECOMPUTE;
}
