#include "resolve.h"

#include <stddef.h>

#include "../../common/arena.h"
#include "../../common/hashmap.h"
#include "../body/body_store.h" // expr_to_id, query_body_store
#include "../index/refs.h"
#include "../modules/modules.h"
#include "../query/query_engine.h"
#include "../sema.h"
#include "../type/checker.h"   // query_type_of_def
#include "../type/decl_data.h" // struct_find_field_def, enum_find_variant_def
#include "../type/type.h"      // struct Type, TY_STRUCT, TY_ENUM
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
  if (!def_id_is_valid(def))
    return false;
  Namespace got = ns_for_semantic(def_semantic_kind(s, def));
  if (want == NS_VALUE_OR_TYPE)
    return got == NS_VALUE || got == NS_TYPE;
  return got == want;
}

// Resolve-ref key: ExprId of the Ident expression + 4-bit namespace.
// Pre-R8 this was `(NodeId << 4) | ns` keyed by the lexer-position
// NodeId — position-shifted on edits. Post-R8 the key is built from
// `expr_id_ns_key(id, ns)` which is stable across sibling-decl
// edits. The Ident's owning decl's body_store walk populates
// expr->expr_id; we look it up via the fast field-read path with
// fallback to expr_to_id for the cold first-access case.
static struct ResolveRefEntry *resolve_ref_entry_for(struct Sema *s,
                                                     struct Expr *ident,
                                                     Namespace ns) {
  if (s->resolve_ref_entries.entries == NULL)
    hashmap_init_in(&s->resolve_ref_entries, &s->arena);

  ExprId id = ident->expr_id;
  if (!expr_id_is_valid(id))
    id = expr_to_id(s, ident);
  if (!expr_id_is_valid(id))
    return NULL;
  uint64_t key = expr_id_ns_key(id, (uint32_t)ns);
  if (hashmap_contains(&s->resolve_ref_entries, key))
    return (struct ResolveRefEntry *)hashmap_get(&s->resolve_ref_entries, key);

  struct ResolveRefEntry *e = arena_alloc(&s->arena, sizeof(*e));
  *e = (struct ResolveRefEntry){.def = DEF_ID_INVALID};
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
                               StrId name_id, Namespace ns) {
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

  struct ResolveRefEntry *entry = resolve_ref_entry_for(s, ident, ns);
  if (!entry)
    return DEF_ID_INVALID;  // synthetic / unreachable Expr

  SEMA_QUERY_GUARD(s, &entry->query, QUERY_RESOLVE_REF, entry, ident->span,
                   /*on_cached=*/entry->def,
                   /*on_cycle=*/DEF_ID_INVALID,
                   /*on_error=*/DEF_ID_INVALID);

  // Record an AST dep so a re-parse invalidates this slot. The slot
  // is keyed by (NodeId, Namespace), but the *ident name* at that
  // node can change between revisions (e.g., `i32` → `u8` at the
  // same position). Without this dep, the cached DefId is served
  // for the new ident and downstream type queries see stale results.
  ModuleId mid = module_for_span(s, ident->span);
  if (module_id_is_valid(mid))
    (void)query_module_ast(s, mid);

  ScopeId enclosing = query_scope_for_node(s, ident);
  StrId name_id = ident->ident.string_id;
  DefId hit = DEF_ID_INVALID;
  if (scope_id_is_valid(enclosing)) {
    if (ns == NS_VALUE_OR_TYPE) {
      // Prefer value, fall back to type. Two walks of the same chain,
      // but inside a single slot (B6: previously the caller did
      // separate query_resolve_ref(NS_VALUE) + query_resolve_ref(NS_TYPE)
      // calls, costing 2× slots, 2× GUARD evaluations, 2× dep
      // recordings on the parent frame). Walk count is unchanged;
      // the win is consolidating slot bookkeeping.
      hit = walk_chain_lookup(s, enclosing, name_id, NS_VALUE);
      if (!def_id_is_valid(hit))
        hit = walk_chain_lookup(s, enclosing, name_id, NS_TYPE);
    } else {
      hit = walk_chain_lookup(s, enclosing, name_id, ns);
    }
  }

  // Emit "name not found" when the chain walk failed. resolve.h
  // documented this as the intended emission site — pending the
  // diag/codes.h dedup work, which we don't actually need for the
  // basic message. Without this, silent failure was producing real
  // footguns like `x = 5` against undeclared `x` accepted at exit 0.
  // Diagnostic lands on this slot's per-slot accumulator so the LSP
  // and CLI both see it via diag_collect_all.
  if (!def_id_is_valid(hit)) {
    const char *name = pool_get(&s->pool, name_id, 0);
    diag_emit(s, ident->span, "cannot find '%s' in scope",
              name ? name : "?");
  }

  entry->def = hit;
  // Fingerprint the resolved DefId so the future invalidator can
  // do early cutoff on the consumer side: if a re-resolve produces
  // the same DefId, downstream queries that consumed this resolve
  // skip recompute.
  query_slot_set_fingerprint(&entry->query,
                             query_fingerprint_from_u64((uint64_t)hit.idx));

  sema_query_succeed(s, &entry->query);
  return hit;
}

// Path resolution: each segment looks up `seg.name` as a member of
// the preceding segment's def, dispatched by kind.
//
//   segment 0: lookup in start_scope → DefId d0
//   segment 1: lookup `seg.name` as a member of d0 → DefId d1
//   ...
//
// Member-lookup rules (per kind of the preceding segment's def):
//   DECL_USER + SEM_TYPE + TY_STRUCT → struct_find_field_def
//   DECL_USER + SEM_TYPE + TY_ENUM   → enum_find_variant_def
//   DECL_USER + SEM_EFFECT        → DEF_ID_INVALID (effect ops not
//                                   wired post-rebuild; see effects R-series)
//   DECL_USER + SEM_MODULE        → DEF_ID_INVALID until @import is wired
//   DECL_IMPORT                   → DEF_ID_INVALID until @import is wired
//                                   (the future home is a per-kind
//                                   `import_data` side table on Sema, with
//                                   the imported ModuleId stored there;
//                                   matches rust-analyzer's per-kind data
//                                   pattern. Today no live producer
//                                   creates a DECL_IMPORT def.)
//   DECL_PRIMITIVE / anything else → DEF_ID_INVALID
//
// rust-analyzer pattern: members of nominal types aren't held in a
// scope (no `ItemScope` for struct/enum); the lookup is direct into
// the signature's fields/variants arena. struct_find_field_def and
// enum_find_variant_def call the relevant signature query so the
// resolve-path slot records a dep on the member set — additions /
// renames invalidate cached path resolutions correctly.
static DefId resolve_member_lookup(struct Sema *s, DefId parent_def,
                                   StrId name) {
  (void)name; // unused until import / module member dispatch lands
  struct DefInfo *di = def_info(s, parent_def);
  if (!di)
    return DEF_ID_INVALID;

  if (di->kind == DECL_USER) {
    SemanticKind sem = def_semantic_kind(s, parent_def);
    if (sem == SEM_TYPE) {
      struct Type *t = query_type_of_def(s, parent_def);
      if (t && t->kind == TY_STRUCT)
        return struct_find_field_def(s, parent_def, name);
      if (t && t->kind == TY_ENUM)
        return enum_find_variant_def(s, parent_def, name);
    }
    // SEM_MODULE / SEM_EFFECT: paths into module exports / effect ops
    // aren't wired yet. Both will dispatch through their per-kind
    // side tables (import_data / effect_signature) when those land.
  }

  return DEF_ID_INVALID;
}

static struct ResolvePathEntry *resolve_path_entry_for(struct Sema *s,
                                                       struct Expr *root,
                                                       Namespace ns) {
  if (s->resolve_path_entries.entries == NULL)
    hashmap_init_in(&s->resolve_path_entries, &s->arena);

  ExprId id = root->expr_id;
  if (!expr_id_is_valid(id))
    id = expr_to_id(s, root);
  if (!expr_id_is_valid(id))
    return NULL;
  uint64_t key = expr_id_ns_key(id, (uint32_t)ns);
  if (hashmap_contains(&s->resolve_path_entries, key))
    return (struct ResolvePathEntry *)hashmap_get(&s->resolve_path_entries,
                                                  key);

  struct ResolvePathEntry *e = arena_alloc(&s->arena, sizeof(*e));
  *e = (struct ResolvePathEntry){.def = DEF_ID_INVALID};
  sema_query_slot_init(&e->query, QUERY_RESOLVE_PATH);
  hashmap_put_or_die(&s->resolve_path_entries, key, e, "resolve_path_entries");
  return e;
}

DefId query_resolve_path(struct Sema *s, struct Expr *root,
                         ScopeId start_scope,
                         const struct PathSegment *segments,
                         size_t segment_count, Namespace ns) {
  if (!root || root->id.id == 0 || segment_count == 0)
    return DEF_ID_INVALID;

  struct ResolvePathEntry *entry = resolve_path_entry_for(s, root, ns);
  if (!entry)
    return DEF_ID_INVALID;
  // Use the first segment's span as the cycle/diag frame span.
  struct Span frame_span = segments[0].span;

  SEMA_QUERY_GUARD(s, &entry->query, QUERY_RESOLVE_PATH, entry, frame_span,
                   /*on_cached=*/entry->def,
                   /*on_cycle=*/DEF_ID_INVALID,
                   /*on_error=*/DEF_ID_INVALID);

  DefId last = DEF_ID_INVALID;
  bool ok = true;

  for (size_t i = 0; i < segment_count; i++) {
    bool is_terminal = (i == segment_count - 1);

    DefId hit;
    if (i == 0) {
      // First segment: chain-walk from the enclosing lexical scope
      // up to the primitives module. walk_chain_lookup records
      // def_map deps at each module/primitives scope it visits.
      if (!scope_id_is_valid(start_scope)) {
        ok = false;
        break;
      }
      Namespace seg_ns = is_terminal ? ns : NS_VALUE;
      hit = walk_chain_lookup(s, start_scope, segments[i].name_id, seg_ns);
    } else {
      // Subsequent segments: look up `seg.name` as a member of the
      // previous def. Per-kind dispatch — for nominal types this
      // routes through the signature query so we record a dep on
      // the member set; for modules / imports we cross into the
      // export scope.
      hit = resolve_member_lookup(s, last, segments[i].name_id);
    }

    if (!def_id_is_valid(hit)) {
      ok = false;
      break;
    }
    last = hit;
  }

  DefId result = ok ? last : DEF_ID_INVALID;
  entry->def = result;
  query_slot_set_fingerprint(&entry->query,
                             query_fingerprint_from_u64((uint64_t)result.idx));
  sema_query_succeed(s, &entry->query);
  return result;
}
