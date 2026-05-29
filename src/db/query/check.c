// CHECK — the per-namespace type-check driver + unused-decl warnings.
//
// This is NOT a memoized query; it is a plain orchestrator over the
// memoized per-decl queries, mirroring rust-analyzer's top-level
// `semantic_diagnostics()` (a plain function that walks salsa-cached
// lower queries). `db_check_namespace` types every decl + infers every
// body — each per-decl query memoizes its result and emits its own
// diagnostics onto its slot's DiagList; the caller then gathers them via
// db_collect_diags_for_file (liveness-gated). A "check" query would have
// no memoizable result and its early-cutoff would save nothing.
//
// Unused-decl warnings are likewise a plain post-pass (emit_unused_warnings),
// recomputed from the CURRENT dependency graph on every check. This is
// deliberate, not a shortcut: salsa's early-cutoff is value-equality on the
// whole memoized output (no fingerprint), so rust-analyzer gets reference-
// change propagation for free (its `InferenceResult` stores resolutions, not
// just types). ore uses hand-rolled fingerprints, so a MEMOIZED usage query
// depending on the per-decl fps would go stale on a same-type reference swap
// (the fp backdates). Recomputing from the fresh dep graph each check sidesteps
// that and is always correct; the expensive per-decl typing stays memoized.

#include "../db.h"
#include "../diag/diag.h"

#include <stdlib.h>

// Query prototypes (the D1 rewrite removed the per-query headers; callers
// re-declare the externs, as type.c / infer.c / scope.c do).
extern FileArray       db_query_namespace_items(db_query_ctx *ctx, NamespaceId nsid);
extern DefId           db_query_def_identity(db_query_ctx *ctx, NamespaceId nsid, AstId id);
extern IpIndex         db_query_type_of_def(db_query_ctx *ctx, DefId def);
extern NodeTypesRange  db_query_infer_body(db_query_ctx *ctx, DefId def);
extern NamespaceScopes db_query_namespace_scopes(db_query_ctx *ctx, NamespaceId nsid);

// The dependency graph IS the reference graph: when type_of_def /
// fn_signature / infer_body resolve a name they call db_query_type_of_def
// on the target, recording a TYPE_OF_DECL dep. So "decl D references X" ⟺
// "type_of_def(X) ∈ D's deps". A decl is unused iff nothing references it
// and it is neither `pub` (exported) nor `main` (the entrypoint).
//
// Plain pass — not memoized. The driver has just ensured each decl's slots
// this revision, so their deps are populated and stable; reading them now
// yields the current reference graph (no staleness). Cleared + re-emitted
// to the namespace's NAMESPACE_SCOPES diag unit each check.
static void emit_unused_warnings(db_query_ctx *ctx, NamespaceId nsid,
                                 FileArray items) {
    struct db *s = (struct db *)ctx;
    const NamespaceItem *a = (const NamespaceItem *)items.data;
    if (items.count == 0)
        return;

    // The unused pass owns this unit's diags (clear + re-emit), so prior
    // warnings don't accumulate across checks.
    db_diags_clear(s, QUERY_NAMESPACE_SCOPES, (uint64_t)nsid.idx);

    size_t ndefs = s->defs.names.count;
    unsigned char *referenced = ndefs ? (unsigned char *)calloc(ndefs, 1) : NULL;
    DefId *defids = (DefId *)malloc((size_t)items.count * sizeof(DefId));

    // Pass 1 — resolve each decl's DefId and union its TYPE_OF_DECL deps
    // (across its three per-decl slots) into the referenced set.
    static const QueryKind kinds[3] = {
        QUERY_TYPE_OF_DECL, QUERY_FN_SIGNATURE, QUERY_INFER_BODY,
    };
    for (uint32_t i = 0; i < items.count; i++) {
        DefId d = db_query_def_identity(ctx, nsid, a[i].id);
        defids[i] = d;
        if (d.idx == 0)
            continue;
        for (int k = 0; k < 3; k++) {
            size_t n = db_slot_dep_count(ctx, kinds[k], (uint64_t)d.idx);
            for (size_t j = 0; j < n; j++) {
                QueryDepRef dep = db_slot_dep_at(ctx, kinds[k], (uint64_t)d.idx, j);
                if (dep.kind == QUERY_TYPE_OF_DECL && dep.key < ndefs)
                    referenced[dep.key] = 1;
            }
        }
    }

    // `main` is the entrypoint — exempt. Sentinel idx 0 ⇒ no "main"
    // interned in this db ⇒ nothing to exempt (the compare stays correct).
    StrId main_name = pool_lookup(&s->strings, "main", 4);

    // Pass 2 — warn on every private decl that nothing references.
    for (uint32_t i = 0; i < items.count; i++) {
        const NamespaceItem *it = &a[i];
        DefId d = defids[i];
        if (it->name.idx == 0 || d.idx == 0)
            continue;
        if ((it->meta & META_VIS_MASK) == VIS_PUBLIC)
            continue;
        if (main_name.idx != 0 && it->name.idx == main_name.idx)
            continue;
        if (d.idx < ndefs && referenced[d.idx])
            continue;

        DiagAnchor anchor = diag_anchor_make((uint16_t)file_id_local(it->file),
                                             it->ptr.kind, it->ptr.range.start,
                                             it->ptr.range.length);
        db_emit_to(s, QUERY_NAMESPACE_SCOPES, (uint64_t)nsid.idx, DIAG_WARNING,
                   anchor, "%S is declared but never used", it->name);
    }

    free(defids);
    free(referenced);
}

// Type-check a whole namespace: type every decl, infer every body (each
// memoized + self-emitting), then emit unused-decl warnings. Caller owns
// the request boundary (db_request_begin/end) and collects diagnostics via
// db_collect_diags_for_file afterward.
void db_check_namespace(db_query_ctx *ctx, NamespaceId nsid) {
    // Ensure the namespace's scope query is live this revision: the unused
    // pass emits its warnings onto the NAMESPACE_SCOPES diag unit, and
    // db_collect_diags_for_file only surfaces diags from a LIVE owner slot.
    // (Done first, before the unused pass clears + re-emits to that unit.)
    (void)db_query_namespace_scopes(ctx, nsid);

    struct db *s = (struct db *)ctx;
    FileArray items = db_query_namespace_items(ctx, nsid);  // db-owned; do not free
    const NamespaceItem *a = (const NamespaceItem *)items.data;

    for (uint32_t i = 0; i < items.count; i++) {
        DefId d = db_query_def_identity(ctx, nsid, a[i].id);
        if (d.idx == 0)
            continue;
        (void)db_query_type_of_def(ctx, d);   // all kinds (fn → also fn_signature)
        // infer_body is KIND_FUNCTION-only at the routing layer — guard so a
        // non-fn decl doesn't trip db_query_begin's "slot kind not wired" assert.
        if (db_def_kind(s, d) == KIND_FUNCTION)
            (void)db_query_infer_body(ctx, d);
    }

    emit_unused_warnings(ctx, nsid, items);
}
