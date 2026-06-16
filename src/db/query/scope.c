// Name layer (Phase D1) — the queries that turn a name into a definition.
//
//   def_identity(nsid, AstId)  — the canonical, reparse-STABLE DefId for a
//                                top-level decl. Mints it once (the interning
//                                pattern, like pool_intern → StrId) and reuses
//                                it forever; AstId keying makes "same kind+name
//                                → same DefId" hold across reparses.
//   namespace_scopes(nsid)     — builds the namespace's `internal` scope
//                                (name → DefId), parented to the primitives
//                                scope, from NAMESPACE_ITEMS.
//   resolve_ref(scope, name)   — resolve a bare identifier by walking the
//                                scope's bindings, then its parent chain.
//
// Pure-query contract: each result column holds a HANDLE (DefId / ScopeId);
// the "body" lives in an owned external store (the defs table for def_identity,
// the scopes table's decl_pool for namespace_scopes) — the same handle-to-
// external-body shape file_imports uses (header in the column, malloc body
// outside). Minting a DefId / appending scope bindings inside a query is the
// monotonic, identity-keyed interning pattern, NOT an untracked side effect.

#define ORE_ENGINE_PRIVATE
#include "capability.h"     // db_write_def_identity, db_write_namespace_scope
#include "engine.h"
#include "engine_internal.h"
#include "result_columns.h" // db.h, ids.h, intern_pool.h, syntax.h

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

// Enumeration source, implemented in parse.c.
extern FileArray db_query_namespace_items(db_query_ctx *ctx, NamespaceId nsid);

// DEF_IDENTITY — the canonical DefId for the decl identified by `id` in
// namespace `nsid`. Keyed by (nsid<<32 | astid) — fully reversible and
// reparse-stable, so the same decl always routes to the same slot whose
// result column holds its DefId. Mints exactly once (interning); recompute
// reuses the stored DefId. fp = DefId.idx (stable) so downstream keyed on
// the DefId cuts off when def_identity merely re-verifies.
DefId db_query_def_identity(db_query_ctx *ctx, NamespaceId nsid, AstId id) {
  struct db *s = (struct db *)ctx;
  uint64_t key = ((uint64_t)nsid.idx << 32) | (uint32_t)id.idx;
  db_query_slot_alloc(ctx, QUERY_DEF_IDENTITY, key);
  DB_QUERY_GUARD(ctx, QUERY_DEF_IDENTITY, key,
                 /* on_cached */ def_identity_read(s, key),
                 /* on_cycle  */ DEF_ID_NONE,
                 /* on_error  */ DEF_ID_NONE);

  // Recover the decl (name + kind) from the items index — binary search,
  // since NAMESPACE_ITEMS is sorted by AstId.
  FileArray items = db_query_namespace_items(ctx, nsid); // records dep
  const NamespaceItem *arr = (const NamespaceItem *)items.data;
  const NamespaceItem *item = NULL;
  for (uint32_t lo = 0, hi = items.count; lo < hi;) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (arr[mid].id.idx < id.idx)
      lo = mid + 1;
    else if (arr[mid].id.idx > id.idx)
      hi = mid;
    else {
      item = &arr[mid];
      break;
    }
  }

  DefId result = DEF_ID_NONE;
  if (item) {
    // Mint once, reuse forever. On a true first-compute the slot's
    // result column is DEF_ID_NONE → mint; on recompute it already
    // holds the DefId → reuse (no churn).
    result = def_identity_read(s, key);
    if (result.idx == 0) {
      result = db_create_def(s);
      // DEF_IDENTITY is the producer: it allocates the def and stamps
      // its three identity columns. The typed setter encodes the
      // safe-co-product contract (same DefId both sides). S1 — the
      // stashed AstId lets DIAG_ANCHOR_BODY emits recover the DeclKey
      // in O(1) from a DefId in hand. id == ast_id_compute(item->kind,
      // item->name) per parse.c's NamespaceItem builder.
      db_write_def_identity(s, result, item->name, nsid, id);
      // No defs.meta: visibility/modifiers live on NamespaceItem.meta
      // (read by the check driver's unused pass); a def's resolved meta is
      // derivable from top_level_entry on demand.
      // No per-def syntax pointer is stored: a decl's location is
      // position-dependent and can't stay fresh behind the membership-fp
      // firewall (def_identity doesn't re-run on a body/shift edit). The
      // CURRENT location is always top_level_entry(nsid, name) — which is
      // why the defs table carries no syntax_ptrs column at all.
    }
    // S1 — item->kind is already the semantic DefKind (the walk classified
    // it straight from the RHS) — no SyntaxKind→DefKind remap here.
    // If the kind has changed (retyping across edits), update it.
    if (result.idx != 0 && item->kind != KIND_NONE) {
      db_def_set_kind(s, result, item->kind);
    }
  }

  def_identity_write(s, key, result);
  db_query_succeed(ctx, QUERY_DEF_IDENTITY, key,
                   result.idx ? db_fp_u64((uint64_t)result.idx)
                              : FINGERPRINT_NONE);
  return result;
}

// NAMESPACE_SCOPES — builds the namespace's `internal` scope: one
// {name, DefId} binding per top-level item, parented to the primitives
// scope. Exports are NOT here — they are the NAMESPACE_TYPE query's member
// list. Deps: NAMESPACE_ITEMS(nsid) + def_identity per decl. fp folds
// (name, DefId) per binding — STABLE across a content edit (same names+DefIds),
// so resolve_ref cuts off; flips on add/remove/rename.
NamespaceScopes db_query_namespace_scopes(db_query_ctx *ctx, NamespaceId nsid) {
  struct db *s = (struct db *)ctx;
  NamespaceScopes empty = {0};
  DB_QUERY_GUARD(ctx, QUERY_NAMESPACE_SCOPES, (uint64_t)nsid.idx,
                 /* on_cached */ namespace_scopes_read(s, nsid),
                 /* on_cycle  */ empty,
                 /* on_error  */ empty);

  FileArray items = db_query_namespace_items(ctx, nsid); // records dep
  const NamespaceItem *arr = (const NamespaceItem *)items.data;

  // Reuse the internal ScopeId across re-runs (stable handle); create once.
  NamespaceScopes prev = namespace_scopes_read(s, nsid);
  ScopeId internal = prev.internal;
  if (internal.idx == 0)
    internal = db_create_scope(s);

  // Append a fresh binding range (the prior range is stranded in decl_pool
  // until compaction — the established rewrite-in-place-(lo,len) pattern).
  // NAMESPACE_SCOPES is the producer of the scope tree + decl_pool —
  // writes here are owned slot updates.
  uint32_t lo = (uint32_t)s->scopes.decl_pool.count;          // LINT_UNTRACKED_OK: producer reads own pool tail
  Fingerprint fp = FINGERPRINT_NONE;
  for (uint32_t i = 0; i < items.count; i++) {
    DefId def = db_query_def_identity(ctx, nsid, arr[i].id); // per-decl dep
    DeclEntry de = {.name = arr[i].name, .def = def};
    vec_push(&s->scopes.decl_pool, &de);                      // LINT_UNTRACKED_OK: producer pool append
    fp = db_fp_combine(fp, db_fp_combine(db_fp_u64((uint64_t)arr[i].name.idx),
                                         db_fp_u64((uint64_t)def.idx)));
  }
  // Auto-import the prelude's members (ore's analogue of Koka's implicit
  // `std/core`): fold them into THIS namespace's internal scope so the
  // primitive effect labels (`asm`, future `io`/aliases) resolve with no
  // `@import`. Own decls were appended first → they SHADOW prelude names
  // (first-match resolve_ref finds a local `asm` before the prelude's).
  // Self-append guard: the prelude doesn't import itself. (Prelude decls are
  // all `pub` by design, so namespace_items == its exports.)
  uint32_t prelude_len = 0;
  if (namespace_id_valid(s->prelude_namespace) &&
      nsid.idx != s->prelude_namespace.idx) {
    FileArray pitems = db_query_namespace_items(ctx, s->prelude_namespace);
    const NamespaceItem *parr = (const NamespaceItem *)pitems.data;
    for (uint32_t i = 0; i < pitems.count; i++) {
      DefId pdef = db_query_def_identity(ctx, s->prelude_namespace, parr[i].id);
      DeclEntry de = {.name = parr[i].name, .def = pdef};
      vec_push(&s->scopes.decl_pool, &de);                      // LINT_UNTRACKED_OK: producer pool append
      fp = db_fp_combine(fp, db_fp_combine(db_fp_u64((uint64_t)parr[i].name.idx),
                                           db_fp_u64((uint64_t)pdef.idx)));
    }
    prelude_len = pitems.count;
  }
  // NAMESPACE_SCOPES owns the scope-row columns for `internal`.
  // Typed setter encodes the safe-co-product contract (same ScopeId
  // across all five columns).
  db_write_namespace_scope(s, internal, s->primitives_scope, SCOPE_MODULE,
                           nsid, lo, items.count + prelude_len);

  NamespaceScopes result = {.internal = internal};
  namespace_scopes_write(s, nsid, result);
  db_query_succeed(ctx, QUERY_NAMESPACE_SCOPES, (uint64_t)nsid.idx, fp);
  return result;
}

// RESOLVE_REF — resolve `name` in `scope`, walking the parent chain. Deps:
// namespace_scopes(scope's owner) so a scope rebuild re-runs this, + the
// parent's resolve. fp = DefId.idx (stable; miss → NONE).
DefId db_query_resolve_ref(db_query_ctx *ctx, ScopeId scope, StrId name) {
  struct db *s = (struct db *)ctx;
  uint64_t key = ((uint64_t)scope.idx << 32) | (uint32_t)name.idx;
  db_query_slot_alloc(ctx, QUERY_RESOLVE_REF, key);
  DB_QUERY_GUARD(ctx, QUERY_RESOLVE_REF, key,
                 /* on_cached */ resolve_ref_read(s, key),
                 /* on_cycle  */ DEF_ID_NONE,
                 /* on_error  */ DEF_ID_NONE);

  DefId result = DEF_ID_NONE;
  if (scope.idx != 0) {
    // Namespace-owned scopes are built by namespace_scopes — depend on
    // it so a rebuild re-runs this lookup. The primitives scope is
    // db_init-built with owning_modules==0, so no dep there.
    // The reads below access NAMESPACE_SCOPES' output. The dep is
    // recorded by the db_query_namespace_scopes call that follows the
    // owner lookup; the lookup itself reads scope-tree state owned by
    // that same producer.
    NamespaceId owner =
        *(NamespaceId *)vec_get(&s->scopes.owning_modules, scope.idx);  // LINT_UNTRACKED_OK: dep recorded next line
    if (namespace_id_valid(owner))
      (void)db_query_namespace_scopes(ctx, owner);

    uint32_t lo = *(uint32_t *)vec_get(&s->scopes.decl_lo, scope.idx);   // LINT_UNTRACKED_OK: dep via namespace_scopes above
    uint32_t len = *(uint32_t *)vec_get(&s->scopes.decl_len, scope.idx); // LINT_UNTRACKED_OK: dep via namespace_scopes above
    const DeclEntry *pool = (const DeclEntry *)s->scopes.decl_pool.data; // LINT_UNTRACKED_OK: dep via namespace_scopes above
    for (uint32_t i = 0; i < len; i++) {
      if (pool[lo + i].name.idx == name.idx) {
        result = pool[lo + i].def;
        break;
      }
    }
    if (result.idx == 0) {
      ScopeId parent = *(ScopeId *)vec_get(&s->scopes.parents, scope.idx); // LINT_UNTRACKED_OK: dep via namespace_scopes above
      if (parent.idx != 0)
        result = db_query_resolve_ref(ctx, parent, name); // parent dep
    }
  }

  resolve_ref_write(s, key, result);
  db_query_succeed(ctx, QUERY_RESOLVE_REF, key,
                   result.idx ? db_fp_u64((uint64_t)result.idx)
                              : FINGERPRINT_NONE);
  return result;
}
