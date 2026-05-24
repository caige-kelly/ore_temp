#include "resolve_ref.h"
#include "../db.h"
#include "def_identity.h"
#include "invalidate.h"
#include "query.h"
#include "query_engine.h"

// Search this scope's decl_pool slice for a DeclEntry whose name
// matches. Returns the entry's index in the GLOBAL decl_pool (so the
// caller can recover the entry's ast_id without re-scanning) or
// UINT32_MAX on miss.
static uint32_t find_in_scope(struct db *s, ScopeId scope, StrId name) {
  if (scope.idx == SCOPE_ID_NONE.idx)
    return UINT32_MAX;
  uint32_t lo = *(uint32_t *)vec_get(&s->scopes.decl_offsets, scope.idx);
  uint32_t hi = *(uint32_t *)vec_get(&s->scopes.decl_offsets, scope.idx + 1);
  for (uint32_t i = lo; i < hi; i++) {
    DeclEntry *de = (DeclEntry *)vec_get(&s->scopes.decl_pool, i);
    if (de->name.idx == name.idx)
      return i;
  }
  return UINT32_MAX;
}

DefId db_query_resolve_ref(struct db *s, ScopeId scope, StrId name) {
  if (scope.idx == SCOPE_ID_NONE.idx || name.idx == 0)
    return DEF_ID_NONE;

  // Route the (scope,name) key to a dense row in db.resolve_ref. The row
  // is allocated once; row 0 is reserved so a real row is non-NULL in the
  // routing HashMap.
  uint64_t k = ((uint64_t)scope.idx << 32) | (uint64_t)name.idx;
  void *rowp = hashmap_get(&s->resolve_ref_cache, k);
  uint32_t row;
  if (!rowp) {
    row = (uint32_t)s->resolve_ref.slots_hot.count;
    vec_push_zero(&s->resolve_ref.results);
    vec_push_zero(&s->resolve_ref.slots_hot);
    vec_push_zero(&s->resolve_ref.slots_cold);
    hashmap_put_or_die(&s->resolve_ref_cache, k, (void *)(uintptr_t)row,
                       "resolve_ref_cache");
  } else {
    row = (uint32_t)(uintptr_t)rowp;
  }

  DB_QUERY_GUARD(s, QUERY_RESOLVE_REF, k,
                 *(DefId *)vec_get(&s->resolve_ref.results, row), DEF_ID_NONE,
                 DEF_ID_NONE);

  // Local lookup first; on miss, fall through to parent scope. The
  // recursive call into db_query_resolve_ref(parent, name) registers a
  // dep on the parent's slot — so an edit that introduces a name in a
  // parent scope correctly invalidates this resolution.
  uint32_t hit = find_in_scope(s, scope, name);
  DefId resolved = DEF_ID_NONE;

  if (hit != UINT32_MAX) {
    // Synthetic-primitives short-circuit. The primitives scope holds
    // pre-allocated DefIds with no AST backing; routing them through
    // db_query_def_identity would scan AstIdMaps that don't contain
    // them. Recover the DefId directly from the decl_pool slot index —
    // the primitive scope's DeclEntries and DefIds share local ordering
    // (see db_init_primitives). No salsa dep needed because primitive
    // identity is immortal.
    if (db_is_primitives_scope(s, scope)) {
      resolved = db_primitive_def_for_slot(s, hit);
    } else {
      DeclEntry *de = (DeclEntry *)vec_get(&s->scopes.decl_pool, hit);
      NamespaceId nsid = *(NamespaceId *)vec_get(&s->scopes.owning_modules, scope.idx);
      // db_query_def_identity records the salsa dep on the identity slot
      // so this resolution invalidates when the resolved decl's identity
      // changes.
      resolved = db_query_def_identity(s, nsid, de->ast_id);
    }
  } else {
    ScopeId parent = *(ScopeId *)vec_get(&s->scopes.parents, scope.idx);
    if (parent.idx != SCOPE_ID_NONE.idx)
      resolved = db_query_resolve_ref(s, parent, name);
  }

  *(DefId *)vec_get(&s->resolve_ref.results, row) = resolved;

  // Fingerprint over (scope, name, resolved_def). The scope+name pair
  // is invariant for this slot key — they're the cache identity — but
  // including them keeps the fp self-describing and matches the
  // identity-tuple pattern used in def_identity.
  Fingerprint fp = db_fp_u64((uint64_t)scope.idx);
  fp = db_fp_combine(fp, db_fp_u64((uint64_t)name.idx));
  fp = db_fp_combine(fp, db_fp_u64((uint64_t)resolved.idx));

  db_query_succeed(s, QUERY_RESOLVE_REF, k, fp);
  return resolved;
}
