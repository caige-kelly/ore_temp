#include "resolve.h"

#include <stddef.h>

#include "../../common/hashmap.h"
#include "../index/refs.h"
#include "../modules/modules.h"
#include "../sema.h"
#include "scope_index.h"

// Resolution algorithm:
//
//   1. Look up the Ident's NodeId in resolved_refs cache; return
//      if present.
//   2. Find its enclosing ScopeId via query_scope_for_node.
//   3. Walk the scope's parent chain. At each scope:
//        a. scope_lookup_local for `name_id`. Hit → cache & return.
//        b. If scope is SCOPE_FUNCTION, also consult
//           query_effect_ops_visible (deferred — empty today).
//   4. After the chain bottoms out, fallback to the prelude's
//      export scope.
//   5. On total miss: cache an invalid DefId and return.
//
// Namespace filtering happens at the lookup layer: scope_lookup
// returns one DefId; we check that its semantic_kind maps to the
// requested namespace before accepting. If not, we keep walking.
// (Multi-namespace scopes are rare; this is the simplest correct
// approach.)

static bool ns_match(struct Sema *s, DefId def, Namespace want) {
  struct DefInfo *di = def_info(s, def);
  if (!di)
    return false;
  return ns_for_semantic(di->semantic_kind) == want;
}

static void cache_resolved(struct Sema *s, struct NodeId node, DefId def) {
  if (node.id == 0)
    return;
  hashmap_put(&s->resolved_refs, (uint64_t)node.id,
              (void *)(uintptr_t)def.idx);
}

static DefId cached_resolved(struct Sema *s, struct NodeId node) {
  if (node.id == 0 || !hashmap_contains(&s->resolved_refs, (uint64_t)node.id))
    return DEF_ID_INVALID;
  void *slot = hashmap_get(&s->resolved_refs, (uint64_t)node.id);
  return (DefId){(uint32_t)(uintptr_t)slot};
}

// Walk the parent chain from `start_scope`, looking for `name_id`
// in the requested namespace. Returns the first match in walk
// order. The prelude is reachable as the topmost ancestor through
// the standard parent-chain traversal — no separate fallback step.
static DefId walk_chain_lookup(struct Sema *s, ScopeId start_scope,
                               uint32_t name_id, Namespace ns) {
  ScopeId cur = start_scope;
  while (scope_id_is_valid(cur)) {
    DefId hit = scope_lookup_local(s, cur, name_id);
    if (def_id_is_valid(hit) && ns_match(s, hit, ns))
      return hit;

    struct ScopeInfo *si = scope_info(s, cur);
    if (!si)
      break;
    cur = si->parent;
  }
  return DEF_ID_INVALID;
}

DefId query_resolve_ref(struct Sema *s, struct Expr *ident, Namespace ns) {
  if (!ident || ident->kind != expr_Ident)
    return DEF_ID_INVALID;

  DefId cached = cached_resolved(s, ident->id);
  if (def_id_is_valid(cached))
    return cached;

  ScopeId enclosing = query_scope_for_node(s, ident->id);
  if (!scope_id_is_valid(enclosing))
    return DEF_ID_INVALID;

  uint32_t name_id = ident->ident.string_id;
  DefId hit = walk_chain_lookup(s, enclosing, name_id, ns);
  // Note: even on miss we cache DEF_ID_INVALID so the lookup is
  // idempotent. Diagnostic emission belongs to the caller for
  // now (sema's call site has the namespace context to phrase
  // the message correctly); the deduping helper in diag/codes.h
  // lands when codes.h ships.
  cache_resolved(s, ident->id, hit);
  // Layer 7.6 — populate the reverse-reference index for LSP
  // "find references" / rename. Only on success; misses don't
  // get tracked (they'd point at the error sentinel).
  if (def_id_is_valid(hit))
    refs_record(s, hit, ident->id);
  return hit;
}

// Path resolution: peel each segment by promoting the resolved
// def to its inhabitable scope.
//
//   segment 0: lookup in start_scope → DefId d0
//   segment 1: lookup in scope-of(d0) → DefId d1
//   ...
//
// Inhabitable scope rules:
//   DECL_USER + SEM_MODULE        → module's export_scope
//   DECL_IMPORT                   → imported module's export_scope
//   DECL_USER + SEM_TYPE          → child_scope (struct/enum members)
//   DECL_USER + SEM_EFFECT        → child_scope (effect ops)
//   DECL_PRIMITIVE                → no child (path stops)
//   anything else                 → no child (path stops)
//
// Visibility check: when crossing a module boundary (DECL_IMPORT
// or moving from one module's export scope to another's), the
// current segment must have Visibility_public.
static ScopeId inhabitable_scope_of(struct Sema *s, DefId def) {
  struct DefInfo *di = def_info(s, def);
  if (!di)
    return SCOPE_ID_INVALID;

  if (di->kind == DECL_IMPORT && module_id_is_valid(di->imported_module))
    return query_module_exports(s, di->imported_module);

  return di->child_scope;
}

DefId query_resolve_path(struct Sema *s, struct NodeId root_node,
                         ScopeId start_scope, const struct PathSegment *segments,
                         size_t segment_count, Namespace ns) {
  DefId cached = cached_resolved(s, root_node);
  if (def_id_is_valid(cached))
    return cached;

  if (segment_count == 0)
    return DEF_ID_INVALID;

  ScopeId cur = start_scope;
  DefId last = DEF_ID_INVALID;

  for (size_t i = 0; i < segment_count; i++) {
    if (!scope_id_is_valid(cur))
      return DEF_ID_INVALID;

    bool is_terminal = (i == segment_count - 1);
    Namespace seg_ns = is_terminal ? ns : NS_VALUE;

    // First segment uses chain-walk semantics (parents up to
    // prelude). Subsequent segments use local-only lookup —
    // dotted paths don't bleed back into outer scopes.
    DefId hit = (i == 0)
        ? walk_chain_lookup(s, cur, segments[i].name_id, seg_ns)
        : scope_lookup_local(s, cur, segments[i].name_id);

    if (!def_id_is_valid(hit))
      return DEF_ID_INVALID;

    last = hit;
    if (is_terminal)
      break;

    cur = inhabitable_scope_of(s, hit);
  }

  cache_resolved(s, root_node, last);
  return last;
}
