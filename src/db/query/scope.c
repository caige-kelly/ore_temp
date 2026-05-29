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
#include "engine.h"
#include "engine_internal.h"
#include "result_columns.h"   // db.h, ids.h, intern_pool.h, syntax.h

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
    FileArray items = db_query_namespace_items(ctx, nsid);  // records dep
    const NamespaceItem *arr = (const NamespaceItem *)items.data;
    const NamespaceItem *item = NULL;
    for (uint32_t lo = 0, hi = items.count; lo < hi; ) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (arr[mid].id.idx < id.idx)      lo = mid + 1;
        else if (arr[mid].id.idx > id.idx) hi = mid;
        else { item = &arr[mid]; break; }
    }

    DefId result = DEF_ID_NONE;
    if (item) {
        // Mint once, reuse forever. On a true first-compute the slot's
        // result column is DEF_ID_NONE → mint; on recompute it already
        // holds the DefId → reuse (no churn).
        result = def_identity_read(s, key);
        if (result.idx == 0) {
            result = db_create_def(s);
            *(StrId *)vec_get(&s->defs.names, result.idx) = item->name;
            *(NamespaceId *)vec_get(&s->defs.parent_modules, result.idx) = nsid;
            *(DefMeta *)vec_get(&s->defs.meta, result.idx) = item->meta;
            // No per-def syntax pointer is stored: a decl's location is
            // position-dependent and can't stay fresh behind the membership-fp
            // firewall (def_identity doesn't re-run on a body/shift edit). The
            // CURRENT location is always top_level_entry(nsid, name) — which is
            // why the defs table carries no syntax_ptrs column at all.
            // item->kind is already the semantic DefKind (the walk classified
            // it straight from the RHS) — no SyntaxKind→DefKind remap here.
            if (item->kind != KIND_NONE)
                db_def_set_kind(s, result, item->kind);
        }
    }

    def_identity_write(s, key, result);
    db_query_succeed(ctx, QUERY_DEF_IDENTITY, key,
                     result.idx ? db_fp_u64((uint64_t)result.idx) : FINGERPRINT_NONE);
    return result;
}

// NAMESPACE_SCOPES — builds the namespace's `internal` scope: one
// {name, DefId} binding per top-level item, parented to the primitives
// scope. `exported` is deferred (subsumed by NAMESPACE_TYPE, D2). Deps:
// NAMESPACE_ITEMS(nsid) + def_identity per decl. fp folds (name, DefId) per
// binding — STABLE across a content edit (same names+DefIds), so resolve_ref
// cuts off; flips on add/remove/rename.
NamespaceScopes db_query_namespace_scopes(db_query_ctx *ctx, NamespaceId nsid) {
    struct db *s = (struct db *)ctx;
    NamespaceScopes empty = {0};
    DB_QUERY_GUARD(ctx, QUERY_NAMESPACE_SCOPES, (uint64_t)nsid.idx,
                   /* on_cached */ namespace_scopes_read(s, nsid),
                   /* on_cycle  */ empty,
                   /* on_error  */ empty);

    FileArray items = db_query_namespace_items(ctx, nsid);  // records dep
    const NamespaceItem *arr = (const NamespaceItem *)items.data;

    // Reuse the internal ScopeId across re-runs (stable handle); create once.
    NamespaceScopes prev = namespace_scopes_read(s, nsid);
    ScopeId internal = prev.internal;
    if (internal.idx == 0)
        internal = db_create_scope(s);

    // Append a fresh binding range (the prior range is stranded in decl_pool
    // until compaction — the established rewrite-in-place-(lo,len) pattern).
    uint32_t lo = (uint32_t)s->scopes.decl_pool.count;
    Fingerprint fp = FINGERPRINT_NONE;
    for (uint32_t i = 0; i < items.count; i++) {
        DefId def = db_query_def_identity(ctx, nsid, arr[i].id);  // per-decl dep
        DeclEntry de = {.name = arr[i].name, .def = def};
        vec_push(&s->scopes.decl_pool, &de);
        fp = db_fp_combine(fp, db_fp_combine(db_fp_u64((uint64_t)arr[i].name.idx),
                                             db_fp_u64((uint64_t)def.idx)));
    }
    *(ScopeId *)vec_get(&s->scopes.parents, internal.idx)      = s->primitives_scope;
    *(ScopeMeta *)vec_get(&s->scopes.meta, internal.idx)       = SCOPE_MODULE;
    *(NamespaceId *)vec_get(&s->scopes.owning_modules, internal.idx) = nsid;
    *(uint32_t *)vec_get(&s->scopes.decl_lo, internal.idx)     = lo;
    *(uint32_t *)vec_get(&s->scopes.decl_len, internal.idx)    = items.count;

    NamespaceScopes result = {.internal = internal, .exported = SCOPE_ID_NONE};
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
        NamespaceId owner =
            *(NamespaceId *)vec_get(&s->scopes.owning_modules, scope.idx);
        if (namespace_id_valid(owner))
            (void)db_query_namespace_scopes(ctx, owner);

        uint32_t lo  = *(uint32_t *)vec_get(&s->scopes.decl_lo, scope.idx);
        uint32_t len = *(uint32_t *)vec_get(&s->scopes.decl_len, scope.idx);
        const DeclEntry *pool = (const DeclEntry *)s->scopes.decl_pool.data;
        for (uint32_t i = 0; i < len; i++) {
            if (pool[lo + i].name.idx == name.idx) {
                result = pool[lo + i].def;
                break;
            }
        }
        if (result.idx == 0) {
            ScopeId parent = *(ScopeId *)vec_get(&s->scopes.parents, scope.idx);
            if (parent.idx != 0)
                result = db_query_resolve_ref(ctx, parent, name);  // parent dep
        }
    }

    resolve_ref_write(s, key, result);
    db_query_succeed(ctx, QUERY_RESOLVE_REF, key,
                     result.idx ? db_fp_u64((uint64_t)result.idx) : FINGERPRINT_NONE);
    return result;
}
