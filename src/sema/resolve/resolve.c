#include "resolve.h"

#include <stddef.h>

#include "../../common/arena.h"
#include "../../common/hashmap.h"
#include "../index/refs.h"
#include "../modules/modules.h"
#include "../query/query_engine.h"
#include "../sema.h"
#include "scope_index.h"

// Resolution algorithm (as a real query):
//
//   1. SEMA_QUERY_GUARD on the per-(NodeId, Namespace) slot. CACHED
//      returns the prior result; CYCLE returns DEF_ID_INVALID; ERROR
//      returns DEF_ID_INVALID.
//   2. Compute path:
//        a. Find the enclosing ScopeId via query_scope_for_node.
//        b. Walk the scope's parent chain. At each top-level
//           (MODULE/PRELUDE) scope, call query_module_def_map on
//           that scope's owner module — this records a dep so the
//           resolve invalidates when the owner module's def-map
//           shape changes (renames, vis flips, adds/removes).
//        c. At each scope, scope_lookup_local for `name_id`. Hit
//           that matches the namespace → success.
//   3. Stamp the result onto the slot's fingerprint and succeed.
//
// Namespace filtering: scope_lookup_local returns one DefId per
// name; we filter by semantic_kind matching the requested namespace.
// Misses (wrong-namespace hits) keep walking parents.
//
// Caching key: `(NodeId<<4) | (uint64_t)ns`. Distinct entries per
// namespace so the same Ident node queried in NS_VALUE vs NS_TYPE
// doesn't share cache state.

static bool ns_match(struct Sema *s, DefId def, Namespace want) {
  struct DefInfo *di = def_info(s, def);
  if (!di)
    return false;
  return ns_for_semantic(di->semantic_kind) == want;
}

static uint64_t resolve_ref_key(struct NodeId node, Namespace ns) {
  // 60 bits of NodeId + 4 bits of Namespace (only 4 namespaces
  // today, room for 12 more before we'd need a wider key).
  return ((uint64_t)node.id << 4) | ((uint64_t)ns & 0xF);
}

static struct ResolveRefEntry *
resolve_ref_entry_for(struct Sema *s, struct NodeId node, Namespace ns) {
  if (s->resolve_ref_entries.entries == NULL)
    hashmap_init_in(&s->resolve_ref_entries, &s->arena);

  uint64_t key = resolve_ref_key(node, ns);
  if (hashmap_contains(&s->resolve_ref_entries, key))
    return (struct ResolveRefEntry *)hashmap_get(&s->resolve_ref_entries, key);

  struct ResolveRefEntry *e = arena_alloc(&s->arena, sizeof(*e));
  *e = (struct ResolveRefEntry){.def = DEF_ID_INVALID,
                                .recorded_def = DEF_ID_INVALID};
  sema_query_slot_init(&e->query, QUERY_RESOLVE_REF);
  hashmap_put_or_die(&s->resolve_ref_entries, key, e, "resolve_ref_entries");
  return e;
}

// Walk the parent chain from `start_scope`, looking for `name_id`
// in the requested namespace. At every top-level scope visited,
// records a dep on `query_module_def_map(scope.owner_module)` so
// the calling resolve_ref slot invalidates when that module's
// def-map shape changes. Local scopes (FUNCTION/BLOCK/HANDLER/
// LOOP/etc.) don't need their own dep call here — we already have
// a transitive dep via query_scope_for_node → query_fn_scope_index
// recorded when this query started.
static DefId walk_chain_lookup(struct Sema *s, ScopeId start_scope,
                               uint32_t name_id, Namespace ns) {
  ScopeId cur = start_scope;
  while (scope_id_is_valid(cur)) {
    struct ScopeInfo *si = scope_info(s, cur);
    if (!si)
      break;

    // Record dep on the producer query for this scope's contents.
    // For module/primitives scopes, that's the def_map of the owner
    // module. After the call, the def_map's name_index is fresh
    // for the current revision and the direct scope_lookup_local
    // below is safe.
    if ((si->kind == SCOPE_MODULE || si->kind == SCOPE_PRIMITIVES) &&
        module_id_is_valid(si->owner_module)) {
      (void)query_module_def_map(s, si->owner_module);
    }

    DefId hit = scope_lookup_local(s, cur, name_id);
    if (def_id_is_valid(hit) && ns_match(s, hit, ns))
      return hit;

    cur = si->parent;
  }
  return DEF_ID_INVALID;
}

DefId query_resolve_ref(struct Sema *s, struct Expr *ident, Namespace ns) {
  if (!ident || ident->kind != expr_Ident || ident->id.id == 0)
    return DEF_ID_INVALID;

  struct ResolveRefEntry *entry = resolve_ref_entry_for(s, ident->id, ns);

  SEMA_QUERY_GUARD(s, &entry->query, QUERY_RESOLVE_REF, entry, ident->span,
                   /*on_cached=*/entry->def,
                   /*on_cycle=*/DEF_ID_INVALID,
                   /*on_error=*/DEF_ID_INVALID);

  // Accumulator drop: if this slot contributed to refs_to_def in a
  // prior run, remove that contribution before re-resolving. Keeps
  // the reverse index consistent when re-resolution lands on a
  // different def (or on no def at all).
  if (def_id_is_valid(entry->recorded_def)) {
    refs_unrecord(s, entry->recorded_def, ident->id);
    entry->recorded_def = DEF_ID_INVALID;
  }

  ScopeId enclosing = query_scope_for_node(s, ident->id);
  uint32_t name_id = ident->ident.string_id;
  DefId hit = scope_id_is_valid(enclosing)
                  ? walk_chain_lookup(s, enclosing, name_id, ns)
                  : DEF_ID_INVALID;

  entry->def = hit;
  // Fingerprint the resolved DefId so the future invalidator can
  // do early cutoff on the consumer side: if a re-resolve produces
  // the same DefId, downstream queries that consumed this resolve
  // skip recompute.
  query_slot_set_fingerprint(&entry->query,
                             query_fingerprint_from_u64((uint64_t)hit.idx));

  // Accumulator add: record the new contribution and remember it
  // on the slot so the next recompute can drop it cleanly.
  if (def_id_is_valid(hit)) {
    refs_record(s, hit, ident->id);
    entry->recorded_def = hit;
  }

  sema_query_succeed(s, &entry->query);
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

static uint64_t resolve_path_key(struct NodeId root_node, Namespace ns) {
  return ((uint64_t)root_node.id << 4) | ((uint64_t)ns & 0xF);
}

static struct ResolvePathEntry *
resolve_path_entry_for(struct Sema *s, struct NodeId root_node, Namespace ns) {
  if (s->resolve_path_entries.entries == NULL)
    hashmap_init_in(&s->resolve_path_entries, &s->arena);

  uint64_t key = resolve_path_key(root_node, ns);
  if (hashmap_contains(&s->resolve_path_entries, key))
    return (struct ResolvePathEntry *)hashmap_get(&s->resolve_path_entries, key);

  struct ResolvePathEntry *e = arena_alloc(&s->arena, sizeof(*e));
  *e = (struct ResolvePathEntry){.def = DEF_ID_INVALID};
  sema_query_slot_init(&e->query, QUERY_RESOLVE_PATH);
  hashmap_put_or_die(&s->resolve_path_entries, key, e, "resolve_path_entries");
  return e;
}

DefId query_resolve_path(struct Sema *s, struct NodeId root_node,
                         ScopeId start_scope,
                         const struct PathSegment *segments,
                         size_t segment_count, Namespace ns) {
  if (root_node.id == 0 || segment_count == 0)
    return DEF_ID_INVALID;

  struct ResolvePathEntry *entry = resolve_path_entry_for(s, root_node, ns);
  // Use the first segment's span as the cycle/diag frame span.
  struct Span frame_span = segments[0].span;

  SEMA_QUERY_GUARD(s, &entry->query, QUERY_RESOLVE_PATH, entry, frame_span,
                   /*on_cached=*/entry->def,
                   /*on_cycle=*/DEF_ID_INVALID,
                   /*on_error=*/DEF_ID_INVALID);

  ScopeId cur = start_scope;
  DefId last = DEF_ID_INVALID;
  bool ok = true;

  for (size_t i = 0; i < segment_count; i++) {
    if (!scope_id_is_valid(cur)) { ok = false; break; }

    bool is_terminal = (i == segment_count - 1);
    Namespace seg_ns = is_terminal ? ns : NS_VALUE;

    // First segment uses chain-walk semantics (parents up to the
    // primitives module). walk_chain_lookup records def_map deps at
    // each module/primitives scope it visits — same dep machinery
    // as query_resolve_ref. Subsequent segments use local-only
    // lookup; their cross-module deps come via inhabitable_scope_of,
    // which calls query_module_exports for DECL_IMPORT crossings.
    DefId hit = (i == 0)
                    ? walk_chain_lookup(s, cur, segments[i].name_id, seg_ns)
                    : scope_lookup_local(s, cur, segments[i].name_id);

    if (!def_id_is_valid(hit)) { ok = false; break; }

    last = hit;
    if (is_terminal)
      break;

    // Peel: cross into the next segment's scope. For DECL_IMPORT,
    // this is the call that records the cross-module dep on
    // query_module_exports(target_module). For type/effect members
    // (DECL_USER + SEM_TYPE/SEM_EFFECT), it returns the def's
    // child_scope — the dep on that scope's contents will be
    // tracked once member queries land in Stage E.2+.
    cur = inhabitable_scope_of(s, hit);
  }

  DefId result = ok ? last : DEF_ID_INVALID;
  entry->def = result;
  query_slot_set_fingerprint(&entry->query,
                             query_fingerprint_from_u64((uint64_t)result.idx));
  sema_query_succeed(s, &entry->query);
  return result;
}
