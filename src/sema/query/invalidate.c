#include "invalidate.h"

#include <stddef.h>

#include "../../common/vec.h"
#include "../modules/def_map.h"
#include "../modules/inputs.h"
#include "../modules/modules.h"
#include "../request/snapshot.h"
#include "../eval/const_eval.h"
#include "../resolve/resolve.h"
#include "../resolve/scope_index.h"
#include "../sema.h"
#include "../type/decl_data.h"
#include "../type/decl_info.h"
#include "../type/expr_check.h"
#include "../type/layout.h"

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
  case QUERY_CONST_EVAL:
    return &((struct ConstEvalEntry *)key)->query;
  case QUERY_TYPE_OF_DECL:
    return &((struct SemaDeclInfo *)key)->type_query;
  case QUERY_RESOLVE_REF:
    return &((struct ResolveRefEntry *)key)->query;
  case QUERY_RESOLVE_PATH:
    return &((struct ResolvePathEntry *)key)->query;
  case QUERY_TYPE_OF_EXPR:
    return &((struct TypeOfExprEntry *)key)->query;
  case QUERY_FN_SIGNATURE:
    return &((struct FnSignature *)key)->query;
  case QUERY_STRUCT_SIGNATURE:
    return &((struct StructSignature *)key)->query;
  case QUERY_ENUM_SIGNATURE:
    return &((struct EnumSignature *)key)->query;
  case QUERY_IS_COMPTIME:
    return &((struct IsComptimeEntry *)key)->query;
  case QUERY_LAYOUT_OF_TYPE:
    return &((struct LayoutEntry *)key)->query;
  case QUERY_TOP_LEVEL_INDEX:
    return &((struct ModuleInfo *)key)->top_level_query;
  case QUERY_INSTANTIATE_DECL:
  case QUERY_EFFECT_SIG:
  case QUERY_BODY_EFFECTS:
  case QUERY_MODULE_FOR_PATH:
  case QUERY_SCOPE_FOR_NODE:
  case QUERY_SCOPE_DECLS:
  case QUERY_SCOPE_PARENT:
  case QUERY_EFFECT_OPS_VISIBLE:
  case QUERY_NODE_TO_DECL:
    // No centrally-addressable slot today. Wire when un-prune
    // brings these queries online.
    return NULL;
  }
  return NULL;
}

RevalidateResult sema_revalidate(struct Sema *s, struct QuerySlot *slot) {
  if (!slot)
    return REVALIDATE_NOT_APPLICABLE;
  // DONE and ERROR are both "we have a recorded result, walk deps to
  // see if it's still valid." A failed query (e.g., name not in scope)
  // captures its frame deps onto the slot via sema_query_fail; if any
  // of those deps' fingerprints have moved, the cause-of-failure may
  // have changed and the caller will recompute.
  if (slot->state != QUERY_DONE && slot->state != QUERY_ERROR)
    return REVALIDATE_NOT_APPLICABLE;

  // Already verified at this revision — fast path. Use the
  // effective revision (request_revision if pinned, else
  // current_revision) so requests under a snapshot see
  // consistent answers even when current_revision moves.
  uint64_t eff = sema_effective_revision(s);
  if (slot->verified_rev == eff)
    return REVALIDATE_SKIP_RECOMPUTE;

#ifdef ORE_DEBUG_QUERIES
  // Salsa's DerivedUntracked memo state. When the slot's compute body
  // read non-query state (signalled via SEMA_READ_UNTRACKED), the
  // recorded deps are insufficient to prove the memo is still valid:
  // an untracked input could have changed without any dep firing. The
  // safe answer is RECOMPUTE — re-execute the body so the new value
  // (and the fresh `has_untracked_read` flag) reflect current state.
  // See bug_of_bugs.md #16, R2; mirrors `validate_memoized_value` ->
  // Stale path for DerivedUntracked in salsa/derived/slot.rs.
  if (slot->has_untracked_read)
    return REVALIDATE_RECOMPUTE;
#endif

  // Walk recorded deps. For each, re-validate it (recursive), then
  // compare its current fingerprint to what we recorded. A mismatch
  // means our cached value is stale.
  if (slot->deps) {
    for (size_t i = 0; i < slot->deps->count; i++) {
      struct QueryDep *dep = (struct QueryDep *)vec_get(slot->deps, i);
      if (!dep)
        continue;

      struct QuerySlot *dep_slot = sema_locate_slot(s, dep->kind, dep->key);
      if (!dep_slot) {
        // Dep is non-addressable; conservatively recompute.
        // (Common when `slot` records deps on queries whose slot
        // dispatch hasn't been wired yet.)
        return REVALIDATE_RECOMPUTE;
      }

      // Recurse. The dep's revalidate returns either:
      //   SKIP_RECOMPUTE       — the dep's deps are all unchanged;
      //                           dep's fingerprint is current and we
      //                           can fingerprint-compare to early-cut.
      //   RECOMPUTE            — at least one transitive dep changed.
      //                           The dep's body hasn't actually re-run
      //                           yet (sema_revalidate doesn't trigger
      //                           recomputation, only sema_query_begin
      //                           does), so we can't trust its current
      //                           fingerprint. Propagate the signal.
      //   NOT_APPLICABLE       — dep's slot wasn't DONE (e.g., already
      //                           reset to EMPTY by set_input_source).
      //                           We catch this via the state check below.
      RevalidateResult dep_result = sema_revalidate(s, dep_slot);
      if (dep_result == REVALIDATE_RECOMPUTE)
        return REVALIDATE_RECOMPUTE;

      // After recursion: if the dep's slot has been externally reset
      // (e.g., set_input_source put ast_query in EMPTY), or if the
      // dep simply has a different fingerprint than what we recorded,
      // our cached value is stale.
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
