// CHECK — the per-namespace type-check driver + unused-decl warnings.
//
// This is NOT a memoized query; it is a plain orchestrator over the
// memoized per-decl queries, mirroring rust-analyzer's top-level
// `semantic_diagnostics()` (a plain function that walks salsa-cached
// lower queries). `db_check_namespace` types every decl + infers every
// body — each per-decl query memoizes its result and emits into its
// own per-query DiagBundle; the caller then gathers them via
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

#define ORE_ENGINE_PRIVATE
#include "capability.h"     // db_get_def_count_untracked (CHECK is a driver)
#include "engine.h"
#include "result_columns.h" // db.h, ids.h, intern_pool.h, syntax.h
#include "../diag/diag.h"

#include <stdlib.h>

// Query prototypes (the D1 rewrite removed the per-query headers; callers
// re-declare the externs, as type.c / infer.c / scope.c do).
extern FileArray db_query_namespace_items(db_query_ctx *ctx, NamespaceId nsid);
extern DefId db_query_def_identity(db_query_ctx *ctx, NamespaceId nsid,
                                   AstId id);
extern IpIndex db_query_type_of_def(db_query_ctx *ctx, DefId def);
extern NodeTypesRange db_query_infer_body(db_query_ctx *ctx, DefId def);
extern FileArray db_query_line_index(db_query_ctx *ctx, FileId fid);
extern const FnBody *db_query_body_scopes(db_query_ctx *ctx, DefId def);

// The dependency graph IS the reference graph: when type_of_def /
// fn_signature / infer_body resolve a name they call db_query_type_of_def
// on the target, recording a TYPE_OF_DECL dep. So "decl D references X" ⟺
// "type_of_def(X) ∈ D's deps". A decl is unused iff nothing references it
// and it is neither `pub` (exported) nor `main` (the entrypoint).
//
// INVARIANT (load-bearing): this equivalence holds only while EVERY name
// resolution forces typing its target (i.e. routes through
// db_query_type_of_def). True for the D2.4 typing surface. A future ref path
// that resolves a top-level name WITHOUT recording a TYPE_OF_DECL dep (e.g. a
// name used in a position that types to IP_NONE before reaching the target, or
// a builtin/comptime path) would be invisible here → a false "unused". If such
// a path is added, give it an explicit reference edge. Self-reference counts as
// "used" (a recursive fn deps on its own type_of_def) — a recursive-but-
// unreachable fn is NOT flagged; this matches the old ref_count behavior and is
// acceptable for D2.
//
// Plain pass — not memoized. The driver has just ensured each decl's slots
// this revision, so their deps are populated and stable; reading them now
// yields the current reference graph (no staleness). Cleared + re-emitted
// to the namespace's CHECK diag unit each check.
static void emit_unused_warnings(db_query_ctx *ctx, NamespaceId nsid,
                                 FileArray items) {
  struct db *s = (struct db *)ctx;
  const NamespaceItem *a = (const NamespaceItem *)items.data;

  // Stamp the dedicated CHECK diag-owner slot live this revision, then clear
  // its prior warnings — BEFORE any early return, so emptying a namespace
  // clears its stale warnings too. CHECK is INPUT-class (set, never computed):
  // nothing db_query_begin's it, so the engine never auto-clears its unit;
  // the driver is the sole owner of clear + emit. This decouples the unused
  // warnings from NAMESPACE_SCOPES' clear-on-recompute lifecycle.
  db_input_set(ctx, QUERY_CHECK, (uint64_t)nsid.idx, FINGERPRINT_NONE, DUR_LOW);
  // Phase P cutover — CHECK owns its own DiagBundle on db.namespaces.
  // Reset on every driver pass (driver is sole owner; no salsa
  // recompute hook is involved). Sink is constructed locally and
  // passed to diag_sink_emit_tagged at each emit site below.
  DiagBundle *check_bundle = check_diags_slot(s, nsid);
  if (check_bundle)
    diag_bundle_reset(check_bundle);
  DiagSink check_sink = check_sink_open(s, nsid);
  if (items.count == 0)
    return;

  // CHECK is the driver — owns the unused-decl pass. Reading the
  // global def count to size the `referenced` bitmap is engine-state,
  // not a query input. Tracked read would over-invalidate every
  // namespace on every new decl anywhere.
  size_t ndefs = db_get_def_count_untracked(s);
  unsigned char *referenced = ndefs ? (unsigned char *)calloc(ndefs, 1) : NULL;
  DefId *defids = (DefId *)malloc((size_t)items.count * sizeof(DefId));
  // OOM — skip the (best-effort) unused-decl pass rather than deref NULL. The
  // CHECK bundle is already reset, so no stale warnings linger. (ndefs == 0
  // leaves `referenced` legitimately NULL; the `< ndefs` guards below stay
  // false, so that case is safe — only a genuine calloc failure bails here.)
  if ((ndefs && !referenced) || !defids) {
    free(referenced);
    free(defids);
    return;
  }

  // Pass 1 — resolve each decl's DefId and union its TYPE_OF_DECL deps
  // (across its three per-decl slots) into the referenced set.
  static const QueryKind kinds[3] = {
      QUERY_TYPE_OF_DECL,
      QUERY_FN_SIGNATURE,
      QUERY_INFER_BODY,
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
    // Exempt the entrypoint `main` — but only a FUNCTION named main; a
    // non-fn `main :: 42` / `main :: struct{}` is not an entrypoint and is
    // still subject to the unused check.
    if (main_name.idx != 0 && it->name.idx == main_name.idx &&
        it->kind == KIND_FUNCTION)
      continue;
    if (d.idx < ndefs && referenced[d.idx])
      continue;

    DiagAnchor anchor =
        diag_anchor_make((uint16_t)file_id_local(it->file), it->ptr.kind, // LINT_FILE_RAW_OK: CHECK driver resets its bundle on every db_check_namespace, so byte offsets are always fresh
                         it->ptr.range.start, it->ptr.range.length);
    // N2 — tag UNNECESSARY so editors render the unused identifier
    // faded/strikethrough instead of a full-strength squiggle.
    // Phase P cutover — explicit sink (CHECK has no query frame).
    diag_sink_emit_tagged(s, &check_sink, DIAG_WARNING,
                          DIAG_TAG_UNNECESSARY, anchor,
                          "%S is declared but never used", it->name);
  }

  free(defids);
  free(referenced);
}

// 7.1 — Redefinition detection. In ONE namespace, two top-level decls with the
// same NAME (any kind) are a redefinition. Items are sorted by AstId and
// same-name decls now share an AstId (name-only identity, 7.0e), so duplicates
// are adjacent. Emit an error at each duplicate site, into the CHECK bundle
// (already reset by emit_unused_warnings this pass; check_sink_open does NOT
// reset). No related-info note — the diag layer omits those by design (diag.h).
static void emit_redefinition_errors(db_query_ctx *ctx, NamespaceId nsid,
                                     FileArray items) {
  struct db *s = (struct db *)ctx;
  const NamespaceItem *a = (const NamespaceItem *)items.data;
  if (items.count < 2)
    return;
  DiagSink sink = check_sink_open(s, nsid);
  for (uint32_t i = 1; i < items.count; i++) {
    const NamespaceItem *cur = &a[i];
    if (cur->name.idx == 0 || cur->name.idx != a[i - 1].name.idx)
      continue;
    DiagAnchor anchor =
        diag_anchor_make((uint16_t)file_id_local(cur->file), cur->ptr.kind, // LINT_FILE_RAW_OK: CHECK driver resets its bundle each db_check_namespace; byte offsets fresh
                         cur->ptr.range.start, cur->ptr.range.length);
    diag_sink_emit_tagged(s, &sink, DIAG_ERROR, DIAG_TAG_NONE, anchor,
                          "redefinition of %S", cur->name);
  }
}

// Walk scope `sc`'s parent chain (fn-local ids); true if `anc` is an ancestor.
static bool scope_is_ancestor(const ScopeRow *scopes, uint32_t anc,
                              uint32_t sc) {
  uint32_t p = scopes[sc].parent;
  while (p != BODY_SCOPE_NONE) {
    if (p == anc)
      return true;
    p = scopes[p].parent;
  }
  return false;
}

// 7.1b + 7.5 — body-scope name conflicts. Per function, scan its FnBody binds:
// two binds in the SAME scope with the same name = redefinition; a bind whose
// name already lives in an ENCLOSING scope = shadowing (forbidden, Zig-style).
// Binds are pushed parents-before-children, so the conflicting earlier bind is
// always at a lower index. O(n²) per fn (bodies are small). Emits into the
// CHECK bundle (reset by emit_unused_warnings). NOTE: only catches conflicts
// WITHIN the fn body — local-over-module-decl / local-over-primitive shadowing
// (Zig also forbids) is a follow-up (needs a namespace resolve per local).
static void emit_body_conflict_errors(db_query_ctx *ctx, NamespaceId nsid,
                                      FileArray items) {
  struct db *s = (struct db *)ctx;
  const NamespaceItem *a = (const NamespaceItem *)items.data;
  DiagSink sink = check_sink_open(s, nsid);
  for (uint32_t it = 0; it < items.count; it++) {
    if (a[it].kind != KIND_FUNCTION || a[it].name.idx == 0)
      continue;
    DefId d = db_query_def_identity(ctx, nsid, a[it].id);
    if (d.idx == 0)
      continue;
    const FnBody *fb = db_query_body_scopes(ctx, d);
    if (!fb || fb->bind_len < 2)
      continue;
    const ScopedBind *binds =
        (const ScopedBind *)s->body_scope_binds.data + fb->bind_off;
    const ScopeRow *scopes =
        (const ScopeRow *)s->body_scope_rows.data + fb->scope_off;
    uint16_t flocal = (uint16_t)file_id_local(a[it].file);
    for (uint32_t i = 1; i < fb->bind_len; i++) {
      const ScopedBind *cur = &binds[i];
      if (cur->name.idx == 0)
        continue;
      for (uint32_t j = 0; j < i; j++) {
        if (binds[j].name.idx != cur->name.idx)
          continue;
        bool same = binds[j].scope_id == cur->scope_id;
        bool shadow =
            !same && scope_is_ancestor(scopes, binds[j].scope_id, cur->scope_id);
        if (!same && !shadow)
          continue;
        DiagAnchor anchor = diag_anchor_make(flocal, cur->bind_site.kind, // LINT_FILE_RAW_OK: CHECK driver resets its bundle each db_check_namespace; offsets fresh
                                             cur->bind_site.range.start,
                                             cur->bind_site.range.length);
        diag_sink_emit_tagged(s, &sink, DIAG_ERROR, DIAG_TAG_NONE, anchor,
                              same ? "redefinition of %S"
                                   : "%S shadows an outer declaration",
                              cur->name);
        break; // one diag per bind
      }
    }
  }
}

// Type-check a whole namespace: type every decl, infer every body (each
// memoized + self-emitting), then emit unused-decl warnings. Caller owns
// the request boundary (db_request_begin/end) and collects diagnostics via
// db_collect_diags_for_file afterward.
void db_check_namespace(db_query_ctx *ctx, NamespaceId nsid) {
  struct db *s = (struct db *)ctx;
  FileArray items =
      db_query_namespace_items(ctx, nsid); // db-owned; do not free
  const NamespaceItem *a = (const NamespaceItem *)items.data;

  for (uint32_t i = 0; i < items.count; i++) {
    DefId d = db_query_def_identity(ctx, nsid, a[i].id);
    if (d.idx == 0)
      continue;
    (void)db_query_type_of_def(ctx, d); // all kinds (fn → also fn_signature)
    // infer_body is KIND_FUNCTION-only at the routing layer — guard so a
    // non-fn decl doesn't trip db_query_begin's "slot kind not wired" assert.
    if (db_def_kind(s, d) == KIND_FUNCTION)
      (void)db_query_infer_body(ctx, d);
  }

  // Multi-file line_index — diag rendering resolves spans via line_starts;
  // a diag anchored in any member file (e.g. an @import'd module) needs that
  // file's index populated this revision, or db_resolve_span returns false
  // and the renderer falls back to bare-message. Cheap: each query is a
  // cache-hit after the first edit. Single-file extension of D2.6b.
  uint32_t nf = 0;
  const FileId *files = db_get_namespace_files(s, nsid, &nf);
  for (uint32_t i = 0; i < nf; i++)
    (void)db_query_line_index(ctx, files[i]);

  emit_unused_warnings(ctx, nsid, items);
  emit_redefinition_errors(ctx, nsid, items);
  emit_body_conflict_errors(ctx, nsid, items);
}
