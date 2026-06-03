// Coerce — Phase H. See coerce.h for the surface + invariants.
//
// Implementation note: the structural rules below are a 1:1 port of the
// can_coerce body that lived in infer.c through Phase F1 (and which was
// itself a 1:1 port of sema_legacy/typechecker/coerce.c at 2938187),
// plus H1.5's concrete-int width-change rules (mirroring Zig Sema.zig
// lines 28897-28905 widening + 29618-29629 narrow-with-range).

#include "coerce.h"

#include "const_eval.h"
#include "../diag/diag.h"
#include "../../ast/ast_expr.h"
#include "../../support/data_structure/hashmap.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

// ------------------------------------------------------------------------
// Effects-3 — row substitution + unification (Koka scoped-labels).
//
// Interned effect rows are immutable; unification CANNOT rewrite them in
// place. Instead, row-variable bindings live in `ctx->row_subst` (HashMap
// row-var id → bound IpIndex.v); every read of a row's tail chases through
// it via subst_resolve.
//
// Phase 3 ships a deliberately-narrow unifier covering the cases the basic
// effects fixtures exercise:
//   - identical rows (trivial)
//   - row var on either side: bind it to the other
//   - closed actual <l1..ln>  vs open expected <m1..mk | μ>: succeed iff
//     {m1..mk} ⊆ {l1..ln} positionally; bind μ to a row holding the
//     residual labels. (Open-closed coercion direction.)
//   - both open with same prefix and a shared bindable tail (one of the
//     tails is a fresh unbound row var).
//
// Cases NOT yet handled and that fall back to strict equality (which may
// reject valid programs):
//   - inner rotation (Koka's positional walk for differing heads in the
//     middle of both rows). The fixtures we ship in Phase 6 don't hit
//     this — both sides currently start aligned or one side is fully
//     open at the tail.
// ------------------------------------------------------------------------

IpIndex row_resolve(const SemaCtx *ctx, IpIndex idx) {
  if (!ctx || !ctx->row_subst || !hashmap_is_initialized(ctx->row_subst))
    return idx;
  for (int hops = 0; hops < 64; hops++) { // guard runaway chains
    IpTag tag = ip_tag(&ctx->s->intern, idx);
    if (tag != IP_TAG_ROW_VAR)
      return idx;
    IpKey k = ip_key(&ctx->s->intern, idx);
    void *bound = hashmap_get(ctx->row_subst, (uint64_t)k.row_var.id);
    if (!bound)
      return idx; // unbound row var — stays as itself
    idx.v = (uint32_t)(uintptr_t)bound;
  }
  return idx; // chain too long — should never happen with single-shot binds
}

static int row_label_cmp(const void *a, const void *b) {
  uint32_t ai = ((const DefId *)a)->idx;
  uint32_t bi = ((const DefId *)b)->idx;
  return (ai > bi) - (ai < bi);
}

// row_flatten — for an effect row whose tail (after row_resolve) is itself
// an effect row, splice that tail's labels into the parent's labels list
// and re-intern. Repeats until the resolved tail is either empty or an
// unbound row var. Pre-conditions: `idx` must be IP_TAG_EFFECT_ROW or
// IP_TAG_ROW_VAR; the result preserves the same tag (effect row) unless
// the row reduces to an unbound row var, in which case that var itself
// is returned. Used at the boundaries of row_unify so a row var bound
// during one unification step is observable as actual labels by a later
// equality / unify check.
IpIndex row_flatten(const SemaCtx *ctx, IpIndex idx) {
  if (!ctx || !ctx->row_subst || !hashmap_is_initialized(ctx->row_subst))
    return idx;
  struct db *s = ctx->s;
  idx = row_resolve(ctx, idx);
  IpTag tag = ip_tag(&s->intern, idx);
  if (tag != IP_TAG_EFFECT_ROW)
    return idx; // unbound row var, etc.
  // Walk down the tail chain, collecting labels into a request-arena
  // scratch buffer. Stop when we hit empty or an unbound row var.
  size_t cap = 8;
  DefId *buf = arena_alloc(&s->request_arena, cap * sizeof(DefId));
  size_t n = 0;
  IpIndex cur = idx;
  IpIndex final_tail = IP_EMPTY_EFFECT_ROW;
  bool any_change = false;
  for (int hops = 0; hops < 64; hops++) {
    IpKey ck = ip_key(&s->intern, cur);
    size_t k_n = ck.effect_row.n_labels;
    if (n + k_n > cap) {
      size_t new_cap = cap;
      while (n + k_n > new_cap) new_cap *= 2;
      DefId *nb = arena_alloc(&s->request_arena, new_cap * sizeof(DefId));
      if (!nb) return idx; // OOM fallback
      for (size_t q = 0; q < n; q++) nb[q] = buf[q];
      buf = nb;
      cap = new_cap;
    }
    for (size_t q = 0; q < k_n; q++) buf[n++] = ck.effect_row.labels[q];
    IpIndex t = row_resolve(ctx, ck.effect_row.tail);
    // If the resolved tail moved (e.g. a row var got bound to ANY row,
    // including IP_EMPTY), force a re-intern. Otherwise a row of shape
    // `<...|μ>` with μ:=<> would slip through unchanged and downstream
    // diag rendering would still print `<..rv#N>`.
    if (t.v != ck.effect_row.tail.v)
      any_change = true;
    IpTag tt = ip_tag(&s->intern, t);
    if (tt == IP_TAG_EFFECT_ROW) {
      if (t.v == IP_EMPTY_EFFECT_ROW.v) {
        final_tail = IP_EMPTY_EFFECT_ROW;
        break;
      }
      // Splice this tail's labels in by continuing the loop with `t`.
      cur = t;
      any_change = true;
      continue;
    }
    // Unbound row var (or some other shape — kept as the new tail).
    final_tail = t;
    break;
  }
  if (!any_change)
    return idx;
  if (n > 1)
    qsort(buf, n, sizeof(DefId), row_label_cmp);
  return row_intern(s, n > 0 ? buf : NULL, n, final_tail);
}

static void subst_bind(const SemaCtx *ctx, uint32_t row_var_id, IpIndex bound) {
  if (!ctx || !ctx->row_subst)
    return;
  if (!hashmap_is_initialized(ctx->row_subst))
    hashmap_init_in(ctx->row_subst, &ctx->s->request_arena);
  hashmap_put_or_die(ctx->row_subst, (uint64_t)row_var_id,
                     (void *)(uintptr_t)bound.v, "row_subst_bind");
}

// occurs_in_row — returns true if the row variable `var_id` appears anywhere
// inside `row` (recursively, chasing through subst_resolve on each row-var
// hop and walking effect-row tails). Used to gate subst_bind in row_unify
// so a cyclic binding (μ := <l | μ>) is rejected with a diag instead of
// silently relying on the chain-length cap to prevent stack overflow.
//
// Walk shape:
//   - row is IP_TAG_ROW_VAR: chase through subst; if the resolved id ==
//     var_id, that's an occurrence. Otherwise (unbound or bound to a non-
//     row-var), continue with the resolved value (could be an effect row).
//   - row is IP_TAG_EFFECT_ROW: recurse on the (resolved) tail. Labels
//     themselves are DefIds, not types — they can't contain a row var.
//   - anything else: not a row-bearing shape, returns false.
//
// Bounded by a 256-hop counter to defend against any pre-existing cycle
// the subst table might already contain (defense-in-depth — once this
// occurs check is in place, no new cycles can be introduced, but an
// already-recorded one shouldn't loop forever here either).
static bool occurs_in_row(const SemaCtx *ctx, uint32_t var_id, IpIndex row) {
  if (!ctx || !ctx->s) return false;
  struct db *s = ctx->s;
  for (int hops = 0; hops < 256; hops++) {
    if (row.v == IP_NONE.v) return false;
    // IP_EMPTY_EFFECT_ROW's tail is itself (the sentinel "no more labels,
    // no row var" terminator from Phase 1). Hitting empty means no row
    // var lives anywhere down this tail — return false BEFORE the
    // EFFECT_ROW recursion which would otherwise loop on the self-ref.
    if (row.v == IP_EMPTY_EFFECT_ROW.v) return false;
    IpTag tag = ip_tag(&s->intern, row);
    if (tag == IP_TAG_ROW_VAR) {
      IpKey k = ip_key(&s->intern, row);
      if (k.row_var.id == var_id) return true;
      // Chase through subst one hop. If unbound, no occurrence.
      if (!ctx->row_subst || !hashmap_is_initialized(ctx->row_subst))
        return false;
      void *bound = hashmap_get(ctx->row_subst, (uint64_t)k.row_var.id);
      if (!bound) return false;
      IpIndex next = {.v = (uint32_t)(uintptr_t)bound};
      if (next.v == row.v) return false; // self-bind sentinel; not a cycle
      row = next;
      continue;
    }
    if (tag == IP_TAG_EFFECT_ROW) {
      IpKey k = ip_key(&s->intern, row);
      IpIndex next = row_resolve(ctx, k.effect_row.tail);
      // Defense-in-depth: if the resolved tail equals the current row
      // (e.g. some future EFFECT_ROW gets a self-tail like IP_EMPTY does),
      // stop here rather than loop. The IP_EMPTY check above handles the
      // common case; this is the general guard.
      if (next.v == row.v) return false;
      row = next;
      continue;
    }
    return false;
  }
  // Hop budget exhausted — treat as "occurs" so the binder rejects rather
  // than tries to extend whatever pre-existing cycle is loitering.
  return true;
}

IpIndex row_intern(struct db *s, const DefId *labels, size_t n,
                   IpIndex tail) {
  IpKey k = {.kind = IPK_EFFECT_ROW,
             .effect_row = {.labels = labels, .n_labels = n, .tail = tail},
             .src_arena = labels ? &s->request_arena : NULL,
             .src_gen = s->request_arena.generation};
  return ip_get(&s->intern, k);
}

// Unify two effect-row IpIndices under ctx->row_subst. Returns true on
// success (possibly after binding tail row vars), false on incompatibility.
// Both inputs are read through row_resolve so chained bindings are
// transparent.
bool row_unify(const SemaCtx *ctx, IpIndex a, IpIndex b) {
  struct db *s = ctx->s;
  // Flatten: if either side's tail (after subst chase) is itself an
  // effect row, splice its labels into the parent and re-intern. This
  // makes a row var that was bound to a concrete row earlier in the
  // frame observable as labels here. Required for the polymorphic
  // apply pattern: after step-1 binds `..e := <io>`, the soundness
  // gate sees `<..e>` vs `<io>` and must flatten `<..e>` to `<io>`
  // before equality / unification can succeed.
  a = row_flatten(ctx, a);
  b = row_flatten(ctx, b);
  if (a.v == b.v)
    return true;

  IpTag at = ip_tag(&s->intern, a);
  IpTag bt = ip_tag(&s->intern, b);

  // Row var on either side → bind it to the other, gated by an occurs
  // check. The 64-hop chain cap in row_resolve prevents stack overflow
  // on a cycle, but never detected the bug — a cyclic binding (μ := <l |
  // μ>) would silently truncate inference and let downstream code see
  // a half-resolved row. occurs_in_row chases through the subst table
  // so binding μ to a row that already mentions μ (directly or via a
  // chain) is reported instead of recorded.
  if (at == IP_TAG_ROW_VAR) {
    IpKey ka = ip_key(&s->intern, a);
    if (occurs_in_row(ctx, ka.row_var.id, b)) {
      // DIAG_ANCHOR_NONE is silently skipped by the diag collector
      // (see src/db/getters/diag.c:43), so we need a non-NONE anchor
      // for the cycle diag to reach the user. row_unify has no
      // SyntaxNode, but ctx->file_local lets us emit a span at the
      // file's start — coarse but visible. Callers with a node
      // could pass a finer anchor in a future refinement.
      db_emit(s, DIAG_ERROR,
              diag_anchor_make((uint16_t)ctx->file_local.idx,
                               SYNTAX_KIND_NONE, 0, 1),
              "cyclic effect row through row variable");
      return false;
    }
    subst_bind(ctx, ka.row_var.id, b);
    return true;
  }
  if (bt == IP_TAG_ROW_VAR) {
    IpKey kb = ip_key(&s->intern, b);
    if (occurs_in_row(ctx, kb.row_var.id, a)) {
      // DIAG_ANCHOR_NONE is silently skipped by the diag collector
      // (see src/db/getters/diag.c:43), so we need a non-NONE anchor
      // for the cycle diag to reach the user. row_unify has no
      // SyntaxNode, but ctx->file_local lets us emit a span at the
      // file's start — coarse but visible. Callers with a node
      // could pass a finer anchor in a future refinement.
      db_emit(s, DIAG_ERROR,
              diag_anchor_make((uint16_t)ctx->file_local.idx,
                               SYNTAX_KIND_NONE, 0, 1),
              "cyclic effect row through row variable");
      return false;
    }
    subst_bind(ctx, kb.row_var.id, a);
    return true;
  }

  // Both must be effect rows now.
  if (at != IP_TAG_EFFECT_ROW || bt != IP_TAG_EFFECT_ROW)
    return false;

  IpKey ka = ip_key(&s->intern, a);
  IpKey kb = ip_key(&s->intern, b);

  // Positional prefix match — labels list is sorted by DefId.idx with
  // duplicates allowed.
  size_t i = 0, j = 0;
  size_t na = ka.effect_row.n_labels, nb = kb.effect_row.n_labels;
  while (i < na && j < nb &&
         ka.effect_row.labels[i].idx == kb.effect_row.labels[j].idx) {
    i++;
    j++;
  }
  bool a_exhausted = (i == na);
  bool b_exhausted = (j == nb);

  // Resolve tails.
  IpIndex a_tail = row_resolve(ctx, ka.effect_row.tail);
  IpIndex b_tail = row_resolve(ctx, kb.effect_row.tail);
  IpTag at_t = ip_tag(&s->intern, a_tail);
  IpTag bt_t = ip_tag(&s->intern, b_tail);

  if (a_exhausted && b_exhausted) {
    // Identical label prefix. Tails must unify.
    return row_unify(ctx, a_tail, b_tail);
  }

  // One side has residual labels — the EXHAUSTED side's tail must be
  // a row variable so we can bind it to swallow the residual labels
  // (plus the other side's tail as continuation). The Koka inner-
  // rotation case (different heads with both tails closed) falls
  // through to failure — see file header.
  //
  // The exhausted side has no residual labels but its tail is the open
  // hole; binding that tail makes the side's effective row include the
  // other side's residual labels, achieving equality. Concretely:
  //   a == b  iff  a_tail (as row var)  →  <b.labels[j..nb] | b_tail>
  //   a == b  iff  b_tail (as row var)  →  <a.labels[i..na] | a_tail>
  //
  // The COMMON polymorphic-apply case (caller arg `<io>` against param
  // `<..e>`) hits the second branch: na=1, nb=0, prefix matches zero,
  // a_exhausted=false, b_exhausted=true, b_tail is a row var → bind
  // b_tail to `<io | empty>` ≡ `<io>`. Subsequent row_resolve on b's
  // tail returns `<io>`, so apply's body row propagates back to its
  // declared row at the caller's soundness gate.
  if (a_exhausted && at_t == IP_TAG_ROW_VAR) {
    IpKey kvar = ip_key(&s->intern, a_tail);
    IpIndex residual =
        row_intern(s, &kb.effect_row.labels[j], nb - j, b_tail);
    subst_bind(ctx, kvar.row_var.id, residual);
    return true;
  }
  if (b_exhausted && bt_t == IP_TAG_ROW_VAR) {
    IpKey kvar = ip_key(&s->intern, b_tail);
    IpIndex residual =
        row_intern(s, &ka.effect_row.labels[i], na - i, a_tail);
    subst_bind(ctx, kvar.row_var.id, residual);
    return true;
  }

  // Closed vs closed (or differing tails with residuals on both sides):
  // not yet handled — falls back to "not unifiable" so callers diag at the
  // structural-mismatch boundary.
  return false;
}

// row_union — produce the canonical merge of two effect rows. Used by
// SK_CALL_EXPR's accumulator (every call's effect row folds into the
// enclosing fn's body row) and by Phase 4a (injecting the parent effect
// into each op's declared row).
//
// Rules (executed in order):
//   1. Resolve both sides through the subst table.
//   2. IP_NONE on either side: propagate IP_NONE (poison).
//   3. Identical IpIndex: return a.
//   4. Either side is IP_EMPTY_EFFECT_ROW: return the other side.
//   5. Either side is a row var: unify it with the other and return the
//      other (the row var now refers to that row via the subst chain).
//   6. Both are effect rows: merge-sort their label lists by DefId.idx
//      (duplicates preserved per Koka's scoped-labels semantics — never
//      dedup), unify the two tails. Intern the result.
//
// On unification failure of the tails, returns IP_NONE so the caller
// can emit a diag at the structural-mismatch boundary.
IpIndex row_union(const SemaCtx *ctx, IpIndex a, IpIndex b) {
  struct db *s = ctx->s;
  if (a.v == IP_NONE.v || b.v == IP_NONE.v)
    return IP_NONE;
  // Flatten so a row var bound to a concrete row earlier in the frame
  // contributes its labels here. Without this, accumulating a callee
  // row that's syntactically `<..e>` but semantically `<io>` (because
  // `..e` was bound at the call's coerce step) silently folds the
  // empty side and loses the concrete labels.
  a = row_flatten(ctx, a);
  b = row_flatten(ctx, b);
  if (a.v == b.v)
    return a;
  if (a.v == IP_EMPTY_EFFECT_ROW.v)
    return b;
  if (b.v == IP_EMPTY_EFFECT_ROW.v)
    return a;

  IpTag at = ip_tag(&s->intern, a);
  IpTag bt = ip_tag(&s->intern, b);

  if (at == IP_TAG_ROW_VAR) {
    if (!row_unify(ctx, a, b))
      return IP_NONE;
    return b;
  }
  if (bt == IP_TAG_ROW_VAR) {
    if (!row_unify(ctx, b, a))
      return IP_NONE;
    return a;
  }

  if (at != IP_TAG_EFFECT_ROW || bt != IP_TAG_EFFECT_ROW)
    return IP_NONE;

  IpKey ka = ip_key(&s->intern, a);
  IpKey kb = ip_key(&s->intern, b);
  size_t na = ka.effect_row.n_labels, nb = kb.effect_row.n_labels;

  // Merge-sort the two ascending-by-DefId.idx label lists. Duplicates
  // (same DefId on both sides) are kept once on each side — Koka's
  // <exn, exn> ≠ <exn>. A label that appears in both rows contributes
  // BOTH instances to the merged list.
  size_t cap = na + nb;
  DefId *out = NULL;
  if (cap > 0) {
    out = arena_alloc(&s->request_arena, cap * sizeof(DefId));
    if (!out)
      return IP_NONE;
  }
  size_t i = 0, j = 0, k = 0;
  while (i < na && j < nb) {
    if (ka.effect_row.labels[i].idx <= kb.effect_row.labels[j].idx)
      out[k++] = ka.effect_row.labels[i++];
    else
      out[k++] = kb.effect_row.labels[j++];
  }
  while (i < na) out[k++] = ka.effect_row.labels[i++];
  while (j < nb) out[k++] = kb.effect_row.labels[j++];

  // Tails: unify recursively. Empty + closed = empty; var + anything =
  // the existing row_unify rules.
  IpIndex a_tail = row_resolve(ctx, ka.effect_row.tail);
  IpIndex b_tail = row_resolve(ctx, kb.effect_row.tail);
  IpIndex merged_tail;
  if (a_tail.v == IP_EMPTY_EFFECT_ROW.v)
    merged_tail = b_tail;
  else if (b_tail.v == IP_EMPTY_EFFECT_ROW.v)
    merged_tail = a_tail;
  else {
    if (!row_unify(ctx, a_tail, b_tail))
      return IP_NONE;
    merged_tail = row_resolve(ctx, a_tail);
  }

  return row_intern(s, out, k, merged_tail);
}

// --- Structural-only sub-check (pure, recursive) --------------------------
//
// Used internally by `coerce()` for the optional-lift recursion. Returns
// true/false on shape match; does not see SyntaxNode or emit diags. Kept
// distinct from the public entrypoint so a maintainer doesn't confuse it
// with an in-memory-equivalence probe.

// =========================================================================
// Effects-4.5 — call-site instantiation + end-of-body defaulting.
// =========================================================================

// Walk a row IpIndex and substitute any IPK_ROW_VAR found through `map`.
// Recursive on EFFECT_ROW tails. `map` is uint32 → uint32 (old row var id
// → new IpIndex.v). Misses mint a fresh row var via ip_fresh_row_var, add
// to map, and recurse. Identical-row optimization: if no row var was
// substituted, returns the input unchanged (no re-intern).
static IpIndex instantiate_row(struct db *s, IpIndex r, HashMap *map) {
  if (r.v == IP_NONE.v || r.v == IP_EMPTY_EFFECT_ROW.v) return r;
  IpTag tag = ip_tag(&s->intern, r);
  if (tag == IP_TAG_ROW_VAR) {
    IpKey k = ip_key(&s->intern, r);
    uint64_t key = (uint64_t)k.row_var.id;
    void *cached = hashmap_get(map, key);
    if (cached) return (IpIndex){.v = (uint32_t)(uintptr_t)cached};
    IpIndex fresh = ip_fresh_row_var(&s->intern);
    hashmap_put(map, key, (void *)(uintptr_t)fresh.v);
    return fresh;
  }
  if (tag != IP_TAG_EFFECT_ROW) return r;
  IpKey k = ip_key(&s->intern, r);
  IpIndex new_tail = instantiate_row(s, k.effect_row.tail, map);
  if (new_tail.v == k.effect_row.tail.v) return r; // no change → reuse
  return row_intern(s, k.effect_row.labels, k.effect_row.n_labels, new_tail);
}

// Walk a type IpIndex and recursively instantiate any row vars found
// inside fn types. Non-fn types are returned unchanged (no row vars
// live in non-fn types in this round). Identical-type optimization
// mirrors instantiate_row.
static IpIndex instantiate_type(struct db *s, IpIndex t, HashMap *map) {
  if (t.v == IP_NONE.v) return t;
  if (ip_tag(&s->intern, t) != IP_TAG_FN_TYPE) return t;
  IpKey k = ip_key(&s->intern, t);

  IpIndex new_ret = instantiate_type(s, k.fn_type.ret, map);
  IpIndex new_er  = instantiate_row(s, k.fn_type.effect_row, map);

  // Instantiate each param. Most params are not fn types and pass
  // through unchanged; allocate the scratch array only when we need
  // a substitution.
  bool params_changed = false;
  IpIndex *new_params = NULL;
  if (k.fn_type.n_params > 0) {
    for (size_t i = 0; i < k.fn_type.n_params; i++) {
      IpIndex orig = k.fn_type.params[i];
      IpIndex sub  = instantiate_type(s, orig, map);
      if (sub.v != orig.v) {
        params_changed = true;
        if (!new_params) {
          new_params = arena_alloc(&s->request_arena,
                                   k.fn_type.n_params * sizeof(IpIndex));
          if (!new_params) return t; // OOM — fall back to identity
          for (size_t j = 0; j < i; j++) new_params[j] = k.fn_type.params[j];
        }
      }
      if (new_params) new_params[i] = sub;
    }
  }

  if (!params_changed && new_ret.v == k.fn_type.ret.v &&
      new_er.v == k.fn_type.effect_row.v)
    return t; // no substitution happened → reuse

  IpKey nk = {.kind = IPK_FN_TYPE,
              .fn_type = {.ret = new_ret,
                          .modifiers = k.fn_type.modifiers,
                          .params = params_changed ? new_params
                                                   : k.fn_type.params,
                          .n_params = k.fn_type.n_params,
                          .effect_row = new_er},
              .src_arena = (params_changed && new_params)
                               ? &s->request_arena
                               : NULL,
              .src_gen = s->request_arena.generation};
  return ip_get(&s->intern, nk);
}

IpIndex instantiate_fn_for_call_site(struct db *s, IpIndex fn_ty) {
  if (fn_ty.v == IP_NONE.v) return fn_ty;
  if (ip_tag(&s->intern, fn_ty) != IP_TAG_FN_TYPE) return fn_ty;
  HashMap map = {0};
  hashmap_init(&map);
  IpIndex result = instantiate_type(s, fn_ty, &map);
  hashmap_free(&map);
  return result;
}

// Walk a row, binding any unbound IPK_ROW_VAR tails to
// IP_EMPTY_EFFECT_ROW in ctx->row_subst. Recursive on EFFECT_ROW
// tails. Called once at end of INFER_BODY for body_row + declared.
void ground_unbound_row_vars(const SemaCtx *ctx, IpIndex r) {
  if (!ctx || !ctx->row_subst) return;
  if (r.v == IP_NONE.v || r.v == IP_EMPTY_EFFECT_ROW.v) return;
  IpIndex resolved = row_resolve(ctx, r);
  IpTag tag = ip_tag(&ctx->s->intern, resolved);
  if (tag == IP_TAG_ROW_VAR) {
    // Unbound after resolve — bind to empty.
    IpKey k = ip_key(&ctx->s->intern, resolved);
    subst_bind(ctx, k.row_var.id, IP_EMPTY_EFFECT_ROW);
    return;
  }
  if (tag != IP_TAG_EFFECT_ROW) return;
  IpKey k = ip_key(&ctx->s->intern, resolved);
  // Recurse on the tail.
  ground_unbound_row_vars(ctx, k.effect_row.tail);
}

// =========================================================================
// Array-init §A — ip_default_value.
//
// Returns the canonical default VALUE IpIndex for `type` (used by
// walk_init_list's {} empty-literal rule), or IP_NONE if `type` has
// no meaningful default. The value is a presence/absence marker for
// sema; codegen reads the TYPE itself when emitting init code.
// =========================================================================
IpIndex ip_default_value(struct db *s, IpIndex type) {
  if (type.v == IP_NONE.v) return IP_NONE;

  // Primitive zero/false defaults — covered by the reserved-value range.
  if (type.v == IP_BOOL_TYPE.v) return IP_BOOL_FALSE;

  // Numeric primitives default to zero. Use IP_ZERO_USIZE as a uniform
  // "presence" marker; codegen reads the actual type to pick the right
  // bit width. Comptime numeric types also accept a zero default (so
  // `c :: [5]comptime_int{}` evaluates to five 0s at comptime).
  if (is_concrete_int(type) || is_concrete_float(type) ||
      type.v == IP_COMPTIME_INT_TYPE.v ||
      type.v == IP_COMPTIME_FLOAT_TYPE.v)
    return IP_ZERO_USIZE;

  IpTag tag = ip_tag(&s->intern, type);

  // Optional types default to nil. Raw pointer / many-ptr / slice
  // explicitly DO NOT — under the strict-nil model (§H), only the `?`
  // wrapper admits nil at the type level.
  if (tag == IP_TAG_OPTIONAL_TYPE)
    return IP_NIL_TYPE;

  // Arrays recurse: [N]T has a default iff T has a default.
  if (tag == IP_TAG_ARRAY_TYPE) {
    IpKey k = ip_key(&s->intern, type);
    return ip_default_value(s, k.array_type.elem);
  }

  // Everything else: no default. Struct fields, enum variants, fn
  // types, effect types, handler types, raw pointers / slices /
  // many-ptrs all fall here. Callers emit a diag pointing at the
  // syntax that asked for a default.
  return IP_NONE;
}

// SemaCtx is threaded through for the effect-row unification path. When
// NULL (or row_subst NULL), the fn-type case falls back to strict effect-
// row equality plus the trivial pure→open admission — which matches the
// pre-Effects-3 behavior for every fn type in the existing fixtures.
static bool coerce_structural_ctx(const SemaCtx *ctx, struct db *s,
                                  IpIndex actual, IpIndex expected) {
  // Cascade-suppression sentinel — IP_NONE is ore's "poisoned type."
  if (actual.v == IP_NONE.v || expected.v == IP_NONE.v)
    return true;
  if (actual.v == expected.v)
    return true;
  // Bottom: noreturn coerces to any type.
  if (actual.v == IP_NORETURN_TYPE.v)
    return true;

  IpTag at = ip_tag(&s->intern, actual);
  IpTag et = ip_tag(&s->intern, expected);

  // Pointer / slice / many-ptr variance: drop mut (X → const X), same elem.
  if ((at == IP_TAG_PTR_TYPE || at == IP_TAG_PTR_CONST_TYPE) &&
      (et == IP_TAG_PTR_TYPE || et == IP_TAG_PTR_CONST_TYPE)) {
    IpKey ak = ip_key(&s->intern, actual), ek = ip_key(&s->intern, expected);
    if (ak.ptr_type.elem.v == ek.ptr_type.elem.v &&
        (at != IP_TAG_PTR_CONST_TYPE || et == IP_TAG_PTR_CONST_TYPE))
      return true;
  }
  if ((at == IP_TAG_SLICE_TYPE || at == IP_TAG_SLICE_CONST_TYPE) &&
      (et == IP_TAG_SLICE_TYPE || et == IP_TAG_SLICE_CONST_TYPE)) {
    IpKey ak = ip_key(&s->intern, actual), ek = ip_key(&s->intern, expected);
    if (ak.slice_type.elem.v == ek.slice_type.elem.v &&
        (at != IP_TAG_SLICE_CONST_TYPE || et == IP_TAG_SLICE_CONST_TYPE))
      return true;
  }
  if ((at == IP_TAG_MANY_PTR_TYPE || at == IP_TAG_MANY_PTR_CONST_TYPE) &&
      (et == IP_TAG_MANY_PTR_TYPE || et == IP_TAG_MANY_PTR_CONST_TYPE)) {
    IpKey ak = ip_key(&s->intern, actual), ek = ip_key(&s->intern, expected);
    if (ak.many_ptr_type.elem.v == ek.many_ptr_type.elem.v &&
        (at != IP_TAG_MANY_PTR_CONST_TYPE ||
         et == IP_TAG_MANY_PTR_CONST_TYPE))
      return true;
  }
  // Array-ptr decay: ^[N]T → []T / [^]T (const flows).
  if (at == IP_TAG_PTR_TYPE || at == IP_TAG_PTR_CONST_TYPE) {
    IpKey ak = ip_key(&s->intern, actual);
    if (ip_tag(&s->intern, ak.ptr_type.elem) == IP_TAG_ARRAY_TYPE) {
      IpKey arrk = ip_key(&s->intern, ak.ptr_type.elem);
      bool a_const = (at == IP_TAG_PTR_CONST_TYPE);
      if (et == IP_TAG_SLICE_TYPE || et == IP_TAG_SLICE_CONST_TYPE) {
        IpKey ek = ip_key(&s->intern, expected);
        if (arrk.array_type.elem.v == ek.slice_type.elem.v &&
            (!a_const || et == IP_TAG_SLICE_CONST_TYPE))
          return true;
      }
      if (et == IP_TAG_MANY_PTR_TYPE || et == IP_TAG_MANY_PTR_CONST_TYPE) {
        IpKey ek = ip_key(&s->intern, expected);
        if (arrk.array_type.elem.v == ek.many_ptr_type.elem.v &&
            (!a_const || et == IP_TAG_MANY_PTR_CONST_TYPE))
          return true;
      }
    }
  }
  // nil fits ?T (any optional). Raw pointers / slices / many-ptrs are
  // non-null; nullability requires the explicit `?` wrapper. Aligns
  // with Zig's safety model: the type system FORCES the user to
  // acknowledge optional unwrapping before dereference, eliminating
  // the silent-landmine class of bugs where a "zero-pointer" looks
  // like valid memory at the type level.
  if (actual.v == IP_NIL_TYPE.v && et == IP_TAG_OPTIONAL_TYPE)
    return true;
  // Optional lift: T → ?T  (recursive on the elem)
  if (et == IP_TAG_OPTIONAL_TYPE) {
    IpKey ek = ip_key(&s->intern, expected);
    if (coerce_structural_ctx(ctx, s, actual, ek.optional_type.elem))
      return true;
  }
  // Phase K — fn-ptr variance. Covariant return, contravariant params,
  // exact modifier match. ore admits fns as first-class values
  // (fn_in_type_position.ore), so this rule fires whenever a fn value
  // crosses an assignability boundary. The standard use case: passing
  // a `fn(^const T) void` where `fn(^T) void` is expected — the
  // contravariant param check unwraps the const-drop variance for us.
  if (at == IP_TAG_FN_TYPE && et == IP_TAG_FN_TYPE) {
    IpKey ak = ip_key(&s->intern, actual), ek = ip_key(&s->intern, expected);
    if (ak.fn_type.n_params == ek.fn_type.n_params &&
        ak.fn_type.modifiers == ek.fn_type.modifiers) {
      // Covariant return: actual.ret coerces TO expected.ret. (Caller
      // sees expected.ret; we may produce a more specific value.)
      if (coerce_structural_ctx(ctx, s, ak.fn_type.ret, ek.fn_type.ret)) {
        bool params_ok = true;
        for (size_t i = 0; i < ak.fn_type.n_params; i++) {
          // Contravariant: expected.params[i] coerces TO actual.params[i].
          // (Caller passes a value that satisfies expected.params[i]; our
          // fn must accept it via actual.params[i].)
          if (!coerce_structural_ctx(ctx, s, ek.fn_type.params[i],
                                     ak.fn_type.params[i])) {
            params_ok = false;
            break;
          }
        }
        if (params_ok) {
          // Effects-3 — fn types must agree on their effect rows too.
          // The unification path is enabled only when an inference frame
          // supplies a row_subst; otherwise we fall back to identity
          // equality (every fn type in the existing codebase has tail
          // IP_EMPTY_EFFECT_ROW, so identity suffices there).
          if (ak.fn_type.effect_row.v == ek.fn_type.effect_row.v)
            return true;
          if (ctx && ctx->row_subst &&
              row_unify(ctx, ak.fn_type.effect_row,
                                ek.fn_type.effect_row))
            return true;
          // Open-closed coercion (Leijen §3.2): a callee with a closed
          // empty effect row may be passed where an open row is demanded.
          // Even without a row_subst we admit the trivial case where the
          // callee is fully pure — the expected side's row var (if any)
          // can absorb the empty residual at the call site.
          if (ak.fn_type.effect_row.v == IP_EMPTY_EFFECT_ROW.v)
            return true;
        }
      }
    }
  }

  // Comptime numeric → concrete (range-check is the caller's job; here we
  // only say the shape matches).
  if (actual.v == IP_COMPTIME_INT_TYPE.v &&
      (is_concrete_int(expected) || is_concrete_float(expected) ||
       expected.v == IP_COMPTIME_FLOAT_TYPE.v))
    return true;
  if (actual.v == IP_COMPTIME_FLOAT_TYPE.v && is_concrete_float(expected))
    return true;

  // H1.5 — concrete-int width-change. Same-tag was caught by the
  // identity check above; here we admit widening that does NOT change
  // sign-interpretation OR signedness (Zig: small-unsigned → wider-
  // signed only). Narrowing requires a const value — handled in
  // `coerce()`, not here.
  if (is_concrete_int(actual) && is_concrete_int(expected)) {
    int af = int_bits(actual), ef = int_bits(expected);
    bool a_unsigned = is_unsigned_int(actual);
    bool a_signed = is_signed_int(actual);
    bool e_unsigned = is_unsigned_int(expected);
    bool e_signed = is_signed_int(expected);
    // Unsigned → unsigned wider: u8 → u16 etc.
    if (a_unsigned && e_unsigned && ef > af)
      return true;
    // Signed → signed wider: i8 → i16 etc.
    if (a_signed && e_signed && ef > af)
      return true;
    // Small-unsigned → strictly-wider-signed: u8 → i16/i32/i64. Zig's
    // rule — same-width unsigned → signed (`u32 → i32`) is rejected
    // because it loses representability for values ≥ 2^31.
    if (a_unsigned && e_signed && ef > af)
      return true;
  }
  return false;
}

// Unwrap any chain of `?` wrappers down to the underlying concrete type.
// `?u8` → `u8`, `??u8` → `u8`. For non-optional input, returns input. Used
// for range-checking — `let x: ?u8 = 1024` should still range-check 1024
// against u8's range, not against the optional shape.
static IpIndex unwrap_optional_chain(struct db *s, IpIndex t) {
  while (ip_tag(&s->intern, t) == IP_TAG_OPTIONAL_TYPE) {
    IpKey k = ip_key(&s->intern, t);
    t = k.optional_type.elem;
  }
  return t;
}

// --- Public entrypoint ----------------------------------------------------

Coercion coerce(const SemaCtx *ctx, SyntaxNode *node, IpIndex actual,
                IpIndex expected) {
  Coercion out = {COERCE_OK, NULL, NULL};

  // Array-init §E — string literal → fixed `[N]u8` buffer.
  //
  // Mirrors C's `char buf[N] = "string"`: bytes 0..L-1 receive the
  // string, bytes L..N-1 default-fill (u8's default is 0). Requires
  // N >= L. There is NO implicit null terminator — for cstring-style
  // use, size the buffer with at least one byte of slack and the
  // u8-default trailing zero stands in.
  //
  // Fires only when:
  //   - `node` is present AND parses as a string-literal SK_LITERAL_EXPR
  //     (handled by ast_string_literal_text — it casts + checks the
  //     literal kind).
  //   - `actual` is IP_STRING_SLICE_TYPE (string literal's static type).
  //   - `expected` is IPK_ARRAY_TYPE with elem == IP_U8_TYPE.
  //
  // The placement BEFORE coerce_structural_ctx is intentional: under
  // strict-nil semantics, `[]const u8 → [N]u8` is otherwise a type
  // mismatch. This rule is the controlled exception.
  if (node && actual.v == IP_STRING_SLICE_TYPE.v &&
      ip_tag(&ctx->s->intern, expected) == IP_TAG_ARRAY_TYPE) {
    IpKey ek = ip_key(&ctx->s->intern, expected);
    if (ek.array_type.elem.v == IP_U8_TYPE.v) {
      const char *txt = NULL;
      uint32_t len = 0;
      if (ast_string_literal_text(node, &txt, &len)) {
        if ((uint64_t)len <= ek.array_type.size) {
          return out; // COERCE_OK — codegen pads tail with zero
        }
        // String too long for the buffer.
        out.kind = COERCE_FAIL_TYPE;
        return out;
      }
    }
  }

  // Structural-first. If shape matches we may still fail FAIL_RANGE
  // for comptime → concrete narrows AND for H1.5 const-narrowing
  // (u16 → u8 where the value fits). The _ctx variant threads SemaCtx so
  // the fn-type case can unify effect rows under ctx->row_subst.
  if (coerce_structural_ctx(ctx, ctx->s, actual, expected)) {
    // Unwrap `?T` chain for range purposes — `comptime_int → ?u8` must
    // range-check the literal against u8's range, not the optional.
    IpIndex range_target = unwrap_optional_chain(ctx->s, expected);
    bool comptime_narrow = (actual.v == IP_COMPTIME_INT_TYPE.v ||
                            actual.v == IP_COMPTIME_FLOAT_TYPE.v) &&
                           (is_concrete_int(range_target) ||
                            is_concrete_float(range_target));
    if (comptime_narrow && node) {
      ConstValue v = db_const_eval(ctx->s, ctx->file_local, node);
      if (v.kind != CONST_NONE) {
        const char *lo = NULL, *hi = NULL;
        if (!db_const_value_fits_in(ctx->s, v, range_target, &lo, &hi)) {
          out.kind = COERCE_FAIL_RANGE;
          out.range_lo = lo;
          out.range_hi = hi;
        }
      }
    }
    return out;
  }

  // H1.5 — concrete-int narrowing succeeds iff we have a const value
  // that fits in `expected`. e.g. `u16 → u8` with the source being a
  // literal `200` is OK; the same coerce on a runtime `u16` is
  // FAIL_TYPE ("use @intCast"). Structural already rejected this width
  // change, so we treat it as a const-only exception. Optional unwrap
  // applies symmetrically so `u16 → ?u8` works the same way.
  IpIndex range_target = unwrap_optional_chain(ctx->s, expected);
  if (is_concrete_int(actual) && is_concrete_int(range_target) && node) {
    ConstValue v = db_const_eval(ctx->s, ctx->file_local, node);
    if (v.kind == CONST_INT) {
      const char *lo = NULL, *hi = NULL;
      if (db_const_value_fits_in(ctx->s, v, range_target, &lo, &hi)) {
        return out; // COERCE_OK
      }
      out.kind = COERCE_FAIL_RANGE;
      out.range_lo = lo;
      out.range_hi = hi;
      return out;
    }
  }

  out.kind = COERCE_FAIL_TYPE;
  return out;
}

bool coerce_or_diag(const SemaCtx *ctx, SyntaxNode *node, IpIndex actual,
                    IpIndex expected) {
  // #20 sticky-error absorption: when either side is the sticky error
  // sentinel, the root diag was already emitted upstream. Return true
  // (silent success) so the caller doesn't cascade a duplicate
  // "expected X, found error" diag.
  if (ip_is_error(actual) || ip_is_error(expected))
    return true;
  Coercion c = coerce(ctx, node, actual, expected);
  if (c.kind == COERCE_OK)
    return true;

  DiagAnchor span = diag_anchor_of_node((uint16_t)ctx->file_local.idx, node);
  if (c.kind == COERCE_FAIL_TYPE) {
    db_emit(ctx->s, DIAG_ERROR, span,
            "expected type '%T', found '%T'", expected, actual);
    return false;
  }
  // FAIL_RANGE — H3 Zig parity strings, keep ore's (range LO..HI) extension.
  ConstValue v = node ? db_const_eval(ctx->s, ctx->file_local, node) : (ConstValue){0};
  char vbuf[64];
  db_const_value_to_str(v, vbuf, sizeof(vbuf));
  if (v.kind == CONST_FLOAT) {
    db_emit(ctx->s, DIAG_ERROR, span,
            "type '%T' cannot represent float value '%s'", expected, vbuf);
  } else if (c.range_lo && c.range_hi) {
    db_emit(ctx->s, DIAG_ERROR, span,
            "type '%T' cannot represent integer value '%s' (range %s..%s)",
            expected, vbuf, c.range_lo, c.range_hi);
  } else {
    db_emit(ctx->s, DIAG_ERROR, span,
            "type '%T' cannot represent integer value '%s'", expected, vbuf);
  }
  return false;
}
