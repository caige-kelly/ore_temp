# Diag-anchor stability audit (Phase 2 baseline → Phase 3 fix)

## Status: SHIPPED. Drift gone for cached-query diags.

| Phase | Result |
|---|---|
| Phase 1 (reproduce) | `tools/lsp_workload.c::scenario_drift_check` reproduces the bug mechanically. Pre-fix output: `FN_SIGNATURE FILE_RAW … ← DRIFT` and `INFER_BODY FILE_RAW … ← DRIFT`. |
| Phase 2 (audit) | 13 emit sites identified. 2 always-fresh (parse.c, check.c), 11 fragile under cached-query caching. See inventory below. |
| Phase 3 (fix) | Unified `DeclAstIdMap` per decl, walked over the entire wrapper. `span_of()` in type.c + infer.c + `coerce_or_diag` all prefer structural `DIAG_ANCHOR_BODY` and fall back to FILE_RAW only when the wrapper-map lookup misses. Drift-check post-fix: `FN_SIGNATURE BODY … ok` and `INFER_BODY BODY … ok` — both anchors structural, no column drift. |
| Lint gate | `tools/lint_untracked_reads.sh` Gate C: new `diag_anchor_make` / `diag_anchor_of_node` calls in `src/db/query/` must carry `// LINT_FILE_RAW_OK <reason>` or use `span_of()`. |
| Regression test | `tools/infer_body_test.c` section (9): edit prepends a comment line at the top of a file with a cached FN_SIGNATURE diag, asserts `col_start` unchanged and `line` shifted by exactly the inserted newline count. |

# Phase 2 — FILE_RAW emit-site audit (preserved for reference)

## Bug context

Squiggle-drift symptom (user report): adding `scoped ` to `Allocator :: pub effect ...` causes UNRELATED downstream red squigglies to drift off position.

Phase 1 (`tools/lsp_workload.c::scenario_drift_check`) mechanically reproduced the bug. Concrete output on a 2-diag fixture:

```
[ 0] FN_SIGNATURE  FILE_RAW  pre=L6:c21(b160)  post=L7:c12(b160)  shift=1/exp=1  ← DRIFT
[ 1] INFER_BODY    FILE_RAW  pre=L11:c12(b274) post=L12:c3(b274)  shift=1/exp=1  ← DRIFT
```

The byte offset (b160, b274) is **identical** in both passes — confirmed FROZEN. The resolver shifts the LINE correctly (because the prepended `// drift\n` has exactly 1 newline) but the COLUMN is wrong because the byte falls at a different position within the new line.

Query-compute deltas during the edit:
```
FILE_AST              1 → 2 (re-ran)
LINE_INDEX            1 → 2 (re-ran)
NAMESPACE_ITEMS       1 → 2 (re-ran)
TOP_LEVEL_ENTRY       3 → 6 (re-ran)
FN_SIGNATURE          —    (cached, NO recompute)
INFER_BODY            —    (cached, NO recompute)
TYPE_OF_DECL          —    (cached, NO recompute)
```

The Phase-3.1 caching of FN_SIGNATURE / INFER_BODY / TYPE_OF_DECL is exactly what leaves the FILE_RAW anchors frozen.

## Anchor system invariants

Two kinds ([diag.h:43-82](src/db/diag/diag.h#L43)):

| Anchor kind | Storage | Resolve behavior | Survives byte shift? |
|---|---|---|---|
| `DIAG_ANCHOR_BODY` | (file_id, DeclKey, RelAstId) | Preorder walk of CURRENT body subtree | ✓ structurally stable |
| `DIAG_ANCHOR_FILE_RAW` | (file_id, byte_start, byte_length) | Lookup byte_start in CURRENT line_index. NO rebind. | ✗ frozen byte offset |

The `BodyAstIdMap` ([ast_id.h:32-35](src/db/diag/ast_id.h#L32)) covers ONLY body-internal nodes — preorder-walked at INFER_BODY compute time and stored on the decl. Nodes OUTSIDE the body (signature, type expressions, decl wrappers) have no equivalent — emit fallback is FILE_RAW.

## Emit-site inventory

13 raw-anchor emit sites in `src/`. Categorized by owning query bundle's reset cadence:

### ALWAYS-FRESH (SAFE — bundle resets every emit)

| Site | Caller / context | Why safe |
|---|---|---|
| [parse.c:45](src/db/query/parse.c#L45) `emit_at_token` | Lexer + parse errors, only inside FILE_AST's frame | FILE_AST recomputes on every byte change → bundle resets → fresh byte anchors. Explicitly documented at the top of parse.c. |
| [check.c:135](src/db/query/check.c#L135) `emit_unused_warnings` | CHECK driver | `db_input_set(QUERY_CHECK)` + `diag_bundle_reset(check_bundle)` fires on every `db_check_namespace` call ([check.c:71-78](src/db/query/check.c#L71)). Driver is sole owner; resets unconditionally. |

### CACHEABLE (FRAGILE — bundle persists across edits, FILE_RAW byte anchors freeze)

| Site | Owning query | Fires when | Drift surface |
|---|---|---|---|
| [infer.c:213](src/db/query/infer.c#L213) `span_of` FILE_RAW fallback | INFER_BODY | `body_ast_id_lookup` misses (sub-query frames, type-expr nodes inside body) | Body-internal type-expression diags, sub-query diags |
| [type.c:193](src/db/query/type.c#L193) `span_of` (unconditional FILE_RAW) | TYPE_OF_DECL, FN_SIGNATURE, build_effect_type, build_struct_type, build_union_type | EVERY emit — no body context available; decl wrappers aren't in any body_ast_id_map | 100% of decl-wrapper-level diags (signature errors, effect-op errors, struct/union field errors, type-resolution errors). This is the HOTTEST site by both volume and drift impact. |
| [coerce.c:251](src/db/query/coerce.c#L251) `row_unify` cycle diag | Caller-dependent (INFER_BODY / TYPE_OF_DECL / FN_SIGNATURE) | Cyclic effect row | Cosmetic — coarse "byte 0" anchor at file start; doesn't shift since byte 0 is stable. Tagged for cleanup, not blocking. |
| [coerce.c:269](src/db/query/coerce.c#L269) | Same as above | Same | Same. |
| [coerce.c:878](src/db/query/coerce.c#L878) `coerce_or_diag` | Usually INFER_BODY | Type mismatch fallback when `node` isn't in body_ast_map | Body-level coerce diags on out-of-walk nodes |
| [const_eval.c:471](src/db/query/const_eval.c#L471) | Whichever query drove const_eval (TYPE_OF_DECL / INFER_BODY) | Circular const dep (`MAX :: MAX`) | Const-cycle diags |
| [const_eval.c:477](src/db/query/const_eval.c#L477) | Same | Const chain too deep | Same. |
| [const_eval.c:941](src/db/query/const_eval.c#L941) | Same | Comptime call to effectful fn | Effectful-call-in-comptime diags |
| [const_eval.c:1071](src/db/query/const_eval.c#L1071) | Same | Field-expr circular dep | Field-expr-cycle diags |
| [const_eval.c:1077](src/db/query/const_eval.c#L1077) | Same | Field-expr chain too deep | Same. |

**11 of 13 emit sites are in cacheable queries.** The fix surface isn't a handful — it's everywhere except the parser and the unused-decl pass.

## Architectural read

The clean structural fix already exists in spirit — `BodyAstIdMap` works by:
1. Preorder-walking a subtree at compute time, building a `SyntaxNodePtr-hash → RelAstId` map ([ast_id.h:32-35](src/db/diag/ast_id.h#L32)).
2. Emit-time: look up the SyntaxNode → RelAstId, store as anchor.
3. Resolve-time: re-walk the CURRENT subtree in preorder, return the i-th node.

The property that makes it work: the preorder walk visits the same logical nodes in the same order whenever the SUBTREE's structural hash is unchanged (sibling reparses leave the subtree intact). RelAstId is structurally invariant under salsa cutoff.

The same construction works for the **decl wrapper**, not just the body. A `DeclWrapperAstIdMap`:
- Built at TYPE_OF_DECL compute time (the moment we have the wrapper SyntaxNode).
- Stored per-decl, like body_ast_id_maps for fns.
- Used by both `infer.c::span_of` (as a second tier after body_ast_map) and `type.c::span_of` (the primary tier — there's no body context).

Resolve property: a sibling edit that doesn't change THIS decl's structural hash leaves both maps stable; node positions can be recovered via the live tree's preorder walk regardless of byte-position shifts.

Sites that would still need FILE_RAW after the structural fix:
- coerce.c's row_unify cycle diags (no SyntaxNode in scope) — coarse "byte 0" is acceptable.
- const_eval's chain-depth diag (`stk->count >= MAX`) — coarse-to-the-decl is fine.

Everything else converts cleanly.

## Tactical fix would not be enough

A "text-length stamp on diag bundle, invalidate on byte shift" band-aid (the original Phase 3 sketch) would:
- Force every cacheable diag-emitting query to re-emit on every byte shift anywhere in the file.
- Lose Phase-3.1's cache benefit for any decl that has ever emitted a sema diag (which is most decls — error path is the common path during active editing).
- Not address the underlying architectural mismatch.

The user's "no wallpaper, no patch" guidance applies here. The structural fix is the right call.

## Recommended fix — extend AstIdMap to decl wrappers

### Scope

~150-200 LOC across:

| File | Change | LOC est. |
|---|---|---|
| `src/db/diag/ast_id.h/.c` | Add `DeclWrapperAstIdMap` mirroring `BodyAstIdMap` shape. Init/free/lookup/walk. | ~80 |
| `src/db/db.h` + per-decl column | Add a `decl_wrapper_ast_id_maps` column keyed by DefId or per-kind storage. Cleared on TYPE_OF_DECL evict. | ~40 |
| `src/db/query/type.c` | At TYPE_OF_DECL compute body, build the wrapper map BEFORE the sema walk. Rewrite `span_of` to look it up. | ~30 |
| `src/db/query/infer.c` | Update `span_of` to fall through to wrapper map after body_ast_map miss. | ~10 |
| `src/db/getters/diag.c` | Extend `diag_resolver_resolve` BODY-anchor path to handle wrapper-relative RelAstIds — or add a third anchor kind `DIAG_ANCHOR_WRAPPER`. | ~30 |
| `src/db/diag/diag.h` | If we add `DIAG_ANCHOR_WRAPPER`, extend the tagged union (DiagAnchor must stay 16 bytes; should still fit). | ~10 |

### Design choice (sub-decision)

Two sub-options for representing wrapper anchors:

**A. Add a third anchor kind: `DIAG_ANCHOR_WRAPPER`.** Clean separation between body-relative and wrapper-relative. Resolver dispatches on kind. Each emit site picks the right kind. **Drawback**: more code at every emit site (have to know if you're in a wrapper or a body).

**B. Generalize `DIAG_ANCHOR_BODY` to "DeclKey + RelAstId where the AstId resolves against whichever map the def has built."** Single kind. The decl owns one map; the resolver picks the map (wrapper map covers more nodes; if the decl has both a wrapper map and a body map, the wrapper map is the parent of the body map). **Pro**: simpler call sites. **Con**: needs careful lifetime/ordering between the two maps.

**Recommendation: Option B.** Simpler emit sites, single resolve path. The maps already nest naturally — the body is a subtree of the wrapper. We can build ONE map per decl that covers wrapper + body in preorder; RelAstId is unique within the whole wrapper.

### Verification

- The Phase-1 drift-check scenario MUST show `→ no drift on common diags` after the fix.
- A new keep-zone test: emit a diag from FN_SIGNATURE, edit a sibling decl's body, resolve the diag → assert col_start matches pre-edit.
- All existing keep-zone gates green (34/34).
- All fixtures green (73/73).
- Phase 3.1 regression gate ([infer_body_test.c (8)](tools/infer_body_test.c)) MUST still pass — INFER_BODY cache hits stay intact.

### Linter beef-up (the user's "catch the issue" request)

Add to `tools/lint_untracked_reads.sh` (or a new lint script) a check that flags any `diag_anchor_make` / `diag_anchor_of_node` call inside `src/db/query/` that lacks a `// LINT_FILE_RAW_OK: <reason>` marker. Mark the SAFE sites (parse.c, check.c) explicitly. Any new site without justification fails lint. This catches regressions on the cacheable-query side.

Pattern mirrors `LINT_UNTRACKED_OK` ([capability.h:37](src/db/query/capability.h#L37)).

## Open questions for the user

1. **Sub-decision A vs B** (single anchor kind that picks the right map, vs separate WRAPPER kind). My recommendation is B.
2. **Should the audit doc check in?** It captures the bug, the surface, and the recommendation — useful future reference. Or kept transient if you prefer not to add docs.
3. **Lint scope**: add the FILE_RAW lint as part of Phase 3, or land Phase 3 first + lint as follow-up?
