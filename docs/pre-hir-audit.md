# Pre-HIR Audit & Cleanup

## Why this exists

We're going to introduce HIR. HIR amplifies whatever's underneath it — clean
foundation pays back, muddy foundation bleeds. This doc inventories what
needs to be audited, in what order, and which findings to act on.

The goal: every part of name resolution and sema that **survives the HIR
transition** should be as close to 100% solid as we can get it before HIR
work begins. Anything HIR will replace, we leave alone — auditing it is
wasted effort.

---

## What HIR replaces (skip these)

Don't sink time auditing things HIR will retire:

- `CheckedBody`'s shape (facts-keyed-by-`Expr*`). HIR replaces with
  typed instructions; the hashmap structure is borrowed time.
- The current shape of `sema_infer_expr`'s big switch returning a
  `Type*`. After HIR, the equivalent emits HIR ops + assigns types as
  part of emission, not as a separate "infer" return.
- The const_eval interface. Already audited thoroughly.
- AST mutation paths the splicer would add. HIR makes them unnecessary.

## Architectural debt (revisit during HIR)

Surfaced during cleanup but explicitly deferred — the HIR transition is
the natural moment to address them, not before:

- **Comptime builtins should route through the query system.**
  `@sizeOf`, `@alignOf`, `@returnType`, `@TypeOf`, `@target.*` are all
  conceptually queries ("compute X about Y at compile time, cache the
  result, detect cycles"), but today they're ad-hoc switch arms in
  `eval_builtin` (const_eval.c) and `expr_Builtin` (checker.c). They
  delegate to real queries underneath (`sema_layout_of_type_at`,
  `sema_infer_expr`), so the heavy work IS cached — but the wrapping
  builtin layer isn't query-shaped, has no per-call-site cache, and no
  uniform "what comptime computations happened?" view. Adding a
  builtin currently touches three files (Sema cached id, init, two
  switches). HIR wants every comptime decision to be a query for
  replay/incremental-rebuild scenarios; routing builtins through it at
  that point is the natural unification. Until then, the current shape
  is acceptable. Don't refactor twice.

---

## What HIR depends on (audit these)

These are the foundations HIR will *read from* during lowering. They need
to be solid:

1. **`Decl` shape and lifecycle.** Every field either fully wired or
   deleted. Today some are half-baked: `is_pub` (TODO-gated),
   `scope_token_id` (correct but inconsistently consulted), `is_export`
   (always true).
2. **`SemaDeclInfo` as the canonical per-decl data.** Sema-time info
   should live here; resolver-time info should live on `Decl`. Verify the
   line is honored.
3. **The query protocol.** `sema_query_begin` / `sema_query_end` used
   uniformly across `TypeOfDecl`, `EffectSig`, `Layout`. Bypass = stale
   results / cycles. HIR adds queries.
4. **Walker completeness.** Every expression kind handled in every
   walker (`resolve_expr_inner`, `sema_infer_expr`, `sema_check_expr`,
   `validate_expr_identifiers`, `tally_expr`,
   `effect_solver::collect_from_expr`).
5. **Fact-recording consistency.** `sema_record_fact` is the contract
   for "sema knows about this expression." Today some paths record, some
   don't. Cleaner now = cleaner migration.
6. **AST is read-only contract.** Identify every place sema mutates the
   AST today. Move to side-tables where cheap; document where expensive.

---

## Audit passes, in order

### Pass 1: `Decl` + `SemaDeclInfo` shape (4-6 hours)

- Inventory every field in `Decl` (`name_resolution.h`) and
  `SemaDeclInfo` (`sema_internal.h`).
- For each: who writes it, who reads it, what it means.
- Mark: keep / delete / move to other struct / promote half-state to
  full-state.
- Expected: 2-3 "should be on `SemaDeclInfo` not `Decl`" or "this is
  dead code" findings.

### Pass 2: Walker completeness (2-3 hours)

- For each expr-kind switch in name_res and sema, list which expr kinds
  are handled.
- Find any holes (would have caught wildcard sooner).
- Don't add `default:` clauses — rely on `-Wswitch`. Just verify all
  are present today.

Files: `name_resolution.c`, `checker.c`, `effect_solver.c`,
`const_eval.c`.

### Pass 3: Query uniformity (2-3 hours)

- List every `sema_query_begin` call site.
- Verify each pairs with `sema_query_end` on every exit path.
- Spot-check cycle handling (error vs returning a placeholder).

### Pass 4: AST mutation map (3-4 hours)

- Grep for everywhere sema writes back to an `Expr*`. Likely candidates:
  `Identifier.resolved`, `Field.field.resolved`, `with.handled_effect`,
  `array_lit.elem_type`, `bind.name.resolved` in re-resolution.
- Document each: what triggers the mutation, why it's currently on the
  AST, where it should live in an HIR world.
- Decide: move to side-tables now (low cost) vs leave for HIR migration.

### Pass 5: Fact-recording audit (2-3 hours)

- Grep for every `sema_record_fact` call.
- Identify expression kinds where facts ARE vs aren't recorded.
- Pattern: every typed expression node records a fact. Find gaps.

### Pass 6 (optional): Memory ownership (4-5 hours)

- For each big struct (`Decl`, `Type`, `SemaDeclInfo`, `CheckedBody`,
  `Module`, `EffectSig`), verify allocation strategy is consistent
  (arena vs heap) and document who owns what.
- More "long-term tech debt" than "blocker for HIR" — defer if
  time-constrained.

---

## Working principle

**One file at a time, leave no stone unturned.** For each file in a pass:

1. Read the whole file front to back.
2. Note every issue: dead code, half-implemented features, inconsistent
   patterns, missing diagnostics, bad naming, weak error handling,
   typos, stale comments.
3. Decide: fix now, defer to a later pass, or document as
   acknowledged-tech-debt.
4. Run `make CC=clang test` after every fix to catch regressions early.

Do NOT fix things HIR will replace. The audit is about the
post-HIR-survivors being clean, not about pre-HIR perfection
everywhere.

---

## Estimated total

- Passes 1-5 (required): **2-3 days**
- Pass 6 (optional): +1 day
- Inline cleanup as findings emerge: woven through

Output: a written record of findings (in this doc, appendix below) and
a series of focused commits applying the fixes.

---

## After the audit

Then HIR work itself, roughly:

1. Define HIR data structures (`HirInstr`, `HirBlock`, `HirFn`, etc.) — 2 days
2. AST→HIR lowering for the easy cases — 3-4 days
3. Lowering for control flow with comptime splicing — 2-3 days
4. Lowering for calls, generic instantiation — 3-4 days
5. Effect handling on HIR — 2-3 days
6. Migrate consumers (codegen scaffold, dump/print) to HIR — 2-3 days
7. Delete the old facts-on-AST path — 1 day

**Total HIR work: 2-3 weeks of focused effort.** Audit upfront keeps
phases 2-4 from accumulating "wait, why is this `Decl` field set here
but not there" rabbit holes.

---

## Findings log

(Populated as audit passes run. Each entry: file, line, category,
note, action.)

### Pass 1 — Type/Decl/Module/Scope/EvidenceFrame field inventory

Inventoried every field in `Decl`, `SemaDeclInfo`, `Module`, `Scope`,
`Resolver`, `Type`, `EffectSig`, `EffectSet`, `EffectTerm`,
`EvidenceFrame`, `EvidenceVector`. Verified read sites for each.

**Diagnostic flow**: confirmed end-to-end. `resolve()` returns a bool
based on diag-bag error-count delta. `sema_check()` returns a bool. Both
exit paths in `main.c` consult `diag_has_errors(&compiler.diags)`.
There's no path that swallows an error silently. Removing
`Resolver.has_errors` is safe.

**Findings & actions**:

| Where | Issue | Action |
| --- | --- | --- |
| `Resolver.has_errors` (name_resolution.h:107) | Set by `resolver_error` and at init; never read. Compiler uses `diag_has_errors(&compiler.diags)`. | **Deleted.** Field, init, and write removed. |
| `Decl.is_comptime` (name_resolution.h:74) | Set on every Decl creation; only read by the resolver dump. `sema_decl_is_comptime` reads `value->is_comptime` (the Bind value's expr flag) instead. | **Wired** into `eval_ident` as the gate for folding non-function bind values. `:=` binds no longer get folded as if they were `::` binds. |
| `Module.exports` (name_resolution.h:88) | Allocated and populated at end of `resolve_module`. No reader anywhere — module member lookups go through `mod->scope->name_index` directly. | **Deleted.** Field, allocation, and harvest loop removed. |
| `EvidenceFrame.with_expr` (effects.h:61) | Set on every frame push in `expr_With`. Comment claims "for diagnostics" but no diagnostic reads it. | **Deleted.** Field and assignment removed. |
| `Decl.is_export` | Always `true` for top-level (TODO-gated until pub-by-default flips). Field works; value is a placeholder. | Keep — TODO at the write site documents the future flip. |

Everything else (`Type.*`, `EffectSig.*`, `EffectSet.*`, `EffectTerm.*`,
`EvidenceVector.*`, `Module.{path_id, scope, ast, resolving, resolved}`,
`Scope.*`, `Resolver.{handler_body_depth, effect_annotation_depth,
next_scope_token_id, with_imports, root_path_id, source_map, diags,
comptime_depth, loop_body_depth, current, root, current_module, ast,
arena, pool, compiler}`) is wired to real consumers.

Tests: 76 passing throughout, no regressions.

### Pass 2 — Walker completeness & quality

#### Inventory

14 expr-kind switches across the codebase. `resolve_expr_inner` and
`sema_infer_expr` cover all 34 kinds. Other walkers handle intentional
subsets. Full table is in the Pass 2 plan
(`/Users/caigekelly/.claude/plans/do-you-mind-auditing-streamed-toast.md`).

#### File: `src/name_resolution/name_resolution.c`

| Location | Severity | Finding | Action |
| --- | --- | --- | --- |
| `is_handler_lifecycle_bind` (lines 693-702) | Low / latent | Uses static `(uint32_t)-1` cache lazily initialized on first call. Cache is global, never invalidated. Fine for single-pool use; silently misdetects if a second compilation in the same process uses a different pool. | Move the interned IDs into `Resolver` so they're per-resolver-instance, computed once at init. |
| `is_builtin_named` (line 195-199) | Low | `pool_intern("import", 6)` runs every call to `is_import_expr`. Pool dedups but the strlen + hashmap lookup is per-call work. | Cache `import` (and any other commonly-checked builtin names) on `Resolver` at init. |
| `expr_With` handler (lines 1029-1195) | Medium | ~165-line case body with 4 heuristics for finding the effect scope. "Convention fallback" (case 3, lines 1109-1127) capitalizes the first letter of the func ident and tries again — string-mangling guess. Brittle, hard to reason about. | Defer to its own commit. Extract the four cases into named helpers; consider replacing the convention fallback with explicit syntax. |
| `collect_effect_scopes` `stack[64]` (line 638) and `expr_With` `stack[16]` (line 1082) | Low / latent | Fixed-size local stack arrays. Silent overflow on deeply nested effect annotations. | Replace with arena-backed `Vec`. ~10 LOC each. |
| Initially/Finally as fake `expr_Bind` (parser side + lines 696-702, 871-888) | Medium / language-design | The parser wraps `initially`/`finally` blocks as `expr_Bind` with the keyword as the bind name. The resolver detects via name comparison and unwraps. This leaks through to const_eval and any future walker. | Promote to a proper AST node (`expr_HandlerLifecycle` with a kind field). Defer to its own commit; touches parser + resolver + AST. |
| Anonymous Lambda at line 1245 vs Bind+Lambda at lines 928-934 | Low | Both paths allocate a fresh `fn_scope` and call `resolve_lambda_into`. Slight code dup. | Extract a `lambda_fn_scope_or_new(decl)` helper. ~10 LOC saved. |
| `dump_scope` indent at lines 1733-1751 | Cosmetic | Mixed-indent printf block reads weird (inconsistent leading spaces). | Re-indent. Trivial. |
| `expr_If` capture comment (lines 810-823) | Cosmetic | Logic is correct (capture binds in then-branch only) but the "else doesn't see the capture" rule could be a one-line comment to make intent explicit. | Add comment. |
| `resolve_expr_inner` falls off switch silently for unknown kinds | Acceptable | No runtime trace; `-Wswitch` catches at build time. Consistent with other walkers. | Keep — `-Wswitch` is the safety net. |
| `validate_expr_identifiers` and `tally_expr` skip Effect/Struct/Enum/EnumRef without an explanatory comment | Low | The exclusion is intentional (these AST nodes don't contain identifier *references* — only definition sites) but a future reader has to figure that out. | Add subset-justification comment above each function. |
| `collect_decl` only handles Bind / DestructureBind | Low | Comment exists ("Anything else at module top level...") but doesn't say which kinds are intentionally excluded. | Add subset-justification comment. |
| `declare_effect_annotation_params` / `validate_effect_annotation_expr` / `collect_effect_scopes` | OK | Each has a comment explaining its narrow purpose. | No change. |

#### File: `src/sema/checker.c`

| Location | Severity | Finding | Action |
| --- | --- | --- | --- |
| `type_display_name` wrapper (was lines 48-50) | Low | Bare wrapper around `sema_type_display_name`. Useless layer — every call could just be the underlying function. | **Deleted.** Rewrote 4 call sites to use `sema_type_display_name` directly. |
| `expr_Return` `result` writes (was lines 1391, 1393, 1403) | Low | Three writes to `result` that were unconditionally overwritten by `result = s->noreturn_type` at the bottom of the case. Pure dead code. | **Removed.** Walk paths kept (so diagnostics inside the value still fire); the dead writes deleted. |
| `expr_Call` duplicate `sema_const_eval_expr` (was line 860-863) | Low | The same comptime call was evaluated twice — once before arg checking (with the return-value-fits check) and once after (just to store `comptime_value`). Redundant work. | **Removed.** Kept the first eval which does the range-check; deleted the second. |
| F1 from plan: `sema_infer_expr` `default:` | N/A — agent inventory was wrong | The Phase-1 inventory claimed `sema_infer_expr` had a `default:` returning `error_type`. There isn't one — only the inner `Bin op` and `Unary op` switches have defaults. Outer expr-kind switch enumerates all 34 kinds and relies on `-Wswitch`. | Skip F1. |
| `expr_Builtin` re-interns name strings per call (lines 533+) | Low | Each `sema_name_is(s, name_id, "import")` re-interns the C string. Pool dedups but per-call work is unnecessary. | Defer. Cache common builtin name IDs on `Sema` at init (mirror the Resolver fix). |
| Unknown @-builtins silently return `unknown_type` in expression position (was lines 587-595) | Medium | A typo'd or missing builtin like `@notabuiltin(x)` produces `unknown_type` and no diagnostic. The const_eval path now errors on unknowns, but the type-checking path doesn't. | Defer. Add an `unknown comptime builtin '@%s'` diagnostic in the unknown-builtin else branch. |
| `expr_If` and `expr_Loop` capture handling (lines 1061-1077, 1348-1364) | Low | Same logic in two places: bind capture decl's type from unwrapped optional, error if non-optional condition. | Defer. Extract a `bind_optional_capture(s, capture, cond_t, span)` helper. ~30 LOC saved. |
| `expr_ArrayType` handler duplicated (lines 1419-1432, 1571-1582) | Low | `sema_infer_expr` and `sema_infer_type_expr` both have an `expr_ArrayType` case with the same `is_many_ptr` branch + `sema_array_type` + `try_set_array_length` logic. | Defer. Extract a helper. |
| `expr_With` sets `frame.scope_token_id = 0` unconditionally (line 1281) | Acknowledged | `infer_scope_param` falls back to `effect_decl->scope_token_id`. Works for single scoped-effect instances; misroutes when two `with` handlers of the same scoped effect are nested. Real bug but the right fix needs codegen context. | Defer to codegen phase. Documented in pre-HIR plan. |
| `expr_Switch` exhaustiveness uses linear scan (lines 1241-1252) | Acceptable | O(N*M) for variants × arms. Fine for typical small enums. | Keep. Revisit only if profiling shows an issue. |
| `expr_Call` `semantic = SEM_VALUE` at line 893 vs TYPE_EFFECT branch | Cosmetic | Subtle: TYPE_EFFECT branch breaks out of the outer switch before line 893 sets `SEM_VALUE`, so the unconditional assignment is correct. Worth a one-line comment. | Defer. Add comment. |

#### File: `src/sema/const_eval.c`

| Location | Severity | Finding | Action |
| --- | --- | --- | --- |
| `sema_const_eval_expr` `default:` (was line 1035) | Medium | Switch handled 19 cases; the 15 unfoldable kinds (Lambda, Struct, Enum, Effect, Ctl, With, EffectRow, Asm, ArrayType, SliceType, ManyPtrType, Defer, EnumRef, DestructureBind, Wildcard) silently fell through to `CONST_INVALID`. Suppressed `-Wswitch` for new kinds. | **F2 applied.** Enumerated all unfoldable kinds explicitly with a `// Unfoldable kinds (intentionally enumerated)` doc comment; dropped the default. Future expr kinds now surface as build warnings until explicitly considered. |
| `eval_loop` fuel = `1000000` magic (was line 724) | Low | Hard-coded interpreter limit. | **Promoted** to `ORE_COMPTIME_LOOP_FUEL` named constant at the top of the file. Diagnostic on overflow now includes the limit. |
| `eval_call` recursion guard `>= 100` magic (was line 659) | Low | Same shape. | **Promoted** to `ORE_COMPTIME_CALL_DEPTH_MAX`. |
| `eval_loop` fuel-exhausted path returned `EVAL_ERROR` silently (was line 728 with `// TODO`) | Low | Stale TODO; user got "couldn't be evaluated" rather than an actionable diagnostic. | **Fixed.** Now emits `comptime loop exceeded fuel (N iterations)` before the error return. |
| Typo `condidition` (was line 777) | Cosmetic | `eval_if` doc comment. | **Fixed.** |
| Typo `propogate` (was line 789) | Cosmetic | `eval_return` doc comment. | **Fixed.** |
| `expr-> block.stmts` extra space (was line 552) | Cosmetic | `eval_block`. | **Fixed.** |
| `eval_array_lit` stale "After confirming your parser's shape, this is the spot to adjust" comment | Cosmetic | Aspirational note from when the array-literal AST shape was in flux. | **Replaced** with a stable rationale comment. |
| `eval_lit` `lit_Nil` returns INVALID | Acknowledged | B1 from prior const_eval audit; explicitly skipped per user direction. | Defer. |
| `eval_target_field` re-interns OS/arch names per call (lines 332-339) | Low | Same intern-per-call as elsewhere; pool dedups but lookup repeats. | Defer. Cache target string IDs on `Sema` when we add the Sema-side intern cache. |
| `is_target_field_chain` re-interns "target" (line 315) | Low | Same. | Defer. |
| `eval_builtin` long if/else over name strings (lines 348-385) | Low | Each `sema_name_is(...)` re-interns. Switch on cached IDs would be cleaner. | Defer (paired with the Sema intern cache). |
| `eval_call` allocates a fresh `ComptimeArgTuple` even on cache hit (line 639+) | Low / latent | Wastes arena per call when the cache hits. Acceptable today since comptime calls are bounded by program text. | Defer. Earlier audit's D3. |
| `eval_field` field-not-found returns INVALID with "type checker should have rejected upstream" comment (lines 887-889) | Acknowledged | Defensive silent path. | Keep. |

#### File: `src/sema/effect_solver.c`

| Location | Severity | Finding | Action |
| --- | --- | --- | --- |
| `collect_from_expr` had `default: return;` without subset-justification | Low | Intentional subset (Call + flow-control kinds), but a future reader had to figure out which kinds were intentionally excluded. | **Doc comment added** above the function explaining the subset and why `default: return;` is correct. |
| `collect_from_call` extracts callee_decl via `if (Ident) ... else if (Field) ...` (lines 97-98) | Low | Duplicates the same logic in `name_resolution::resolved_decl_for_expr` and `effects::resolved_decl_from_effect_expr`. | Defer. Promote one of them to a shared `ast.h` (or `decls.h`) helper. |
| TODO(perf) at line 30 (linear-scan EffectSet membership) | Acknowledged | Comment notes the threshold. | Keep. |
| Public queries (`sema_effect_sig_of_callable`, `sema_body_effects_of`) use `sema_query_begin/succeed` correctly | OK | Uniform query pattern. | No change. |

#### File: `src/sema/effects.c`

| Location | Severity | Finding | Action |
| --- | --- | --- | --- |
| `resolved_decl_from_effect_expr` / `name_id_from_effect_expr` (lines 21-37) | Low | Same Ident/Field decl-extraction pattern as in `name_resolution.c` and `effect_solver.c`. Three copies. | Defer. Same shared-helper opportunity as effect_solver. |
| `effect_sig_collect_term` Bin non-Pipe falls through to UNKNOWN (line 80 → 113-117) | Low | The `break;` on non-Pipe Bin lands at the post-switch UNKNOWN-push block. Works but the data-flow is implicit. | Defer. Make non-Pipe Bin an explicit case that produces UNKNOWN, mirror clarity of the EffectRow/Call cases. |
| `scope_token_id_from_args` only checks Ident args (line 44) | Acceptable | Scope tokens are always identifiers in current syntax. | Keep. |
| Cache uses `Expr*` identity (line 9-11) | Acknowledged | Same as CheckedBody facts; HIR will replace the keying scheme. | Keep — HIR will rewrite. |

#### File: `src/parser/parser.c` (`print_ast` only)

| Location | Severity | Finding | Action |
| --- | --- | --- | --- |
| `print_ast` `default:` prints `<unhandled expr kind: N>` (line 369) | Acknowledged | Suppresses `-Wswitch` but provides a runtime trace. For a developer dump tool the runtime trace is the right shape; new kinds show up visibly in dump output. | **Doc comment added** explaining the deliberate trade-off. |

### Pass 3 — Query uniformity

#### Inventory

5 `sema_query_begin` call sites, all using the standard begin/succeed/fail
protocol:

| Site | Query kind | Pairing |
| --- | --- | --- |
| `decls.c:323` (`compute_decl_signature`) | `QUERY_TYPE_OF_DECL` | succeed + fail both reachable on COMPUTE path |
| `layout.c:189` (`sema_layout_of_type_at`) | `QUERY_LAYOUT_OF_TYPE` | succeed + fail both reachable on COMPUTE path |
| `effect_solver.c:244` (`sema_effect_sig_of_callable`) | `QUERY_EFFECT_SIG` | succeed only (intentional — see comment) |
| `effect_solver.c:267` (`sema_body_effects_of`) | `QUERY_BODY_EFFECTS` | succeed only (intentional — see comment) |
| `instantiate.c:256` (`sema_instantiate_decl`) | `QUERY_INSTANTIATE_DECL` | succeed only (intentional — see comment) |

CACHED / ERROR / CYCLE branches return early without calling
succeed/fail, which is correct: those branches don't push the query
stack so they shouldn't pop it. Spot-checked the stack discipline by
tracing each branch — no drift.

#### Findings & actions

| Location | Severity | Finding | Action |
| --- | --- | --- | --- |
| `QueryKind` enum: `QUERY_SIGNATURE_OF_DECL`, `QUERY_CONST_EVAL_EXPR`, `QUERY_EVIDENCE_AT`, `QUERY_EVIDENCE_OF_BODY` (query.h:19-26) | Low | All four enum values are declared and named in `sema_query_kind_str` but never passed to `sema_query_begin`. Aspirational placeholders. | **Deleted** all four from the enum and the kind-string switch. New queries are easy to add when they actually exist. |
| `QuerySlot.cycle_reported` (query.h:39) | Low | Field set in `sema_query_begin` (CYCLE branch + COMPUTE entry) and in `sema_query_succeed`; **never read anywhere**. Three writes, zero reads. | **Deleted** the field, the init, and the three writes. |
| Three "succeed-only" call sites without explanatory comment (`sema_effect_sig_of_callable`, `sema_body_effects_of`, `sema_instantiate_decl`) | Cosmetic | Pairing is intentional — these queries treat NULL/error results as cached values rather than failures, so future calls don't re-emit the same diagnostics. But a future reader has to figure that out. | **Added** `// No fail path:` doc comments to all three explaining the semantics. |
| Stack discipline | OK | Every COMPUTE-returning `sema_query_begin` is followed by exactly one `succeed` or `fail` before the function returns. Verified per call site. | No change. |
| Cycle handling | OK | The CYCLE return is consistent across all five sites: report a diagnostic (`compute_decl_signature`, `layout_of_type_at`, `instantiate_decl`) or return a sentinel value (`effect_sig_of_callable`, `body_effects_of`). All five eventually let the originating call complete and pop the stack. | No change. |

Tests: 76 passing throughout.

### Pass 4 — AST mutation map

#### Inventory

Searched all of `src/sema/` and `src/name_resolution/` for writes to AST
node fields. Result:

**Resolver writes (expected — name resolution's job):**

| Site | Field | Trigger |
| --- | --- | --- |
| `name_resolution.c:126` | `Decl.name.resolved = d` (self-ref) | Decl allocation |
| `name_resolution.c:724` | `Identifier.resolved` on `expr_Ident` | Pass 2 ident resolution |
| `name_resolution.c:790` | `Identifier.resolved` on `expr_EffectRow.row` | Pass 2 effect-row resolution |
| `name_resolution.c:1177` | `with.handled_effect` | Pass 2 — caches discovered effect on the AST so sema doesn't re-search |
| `name_resolution.c:1204` | `Identifier.resolved` on `expr_Field.field` | Pass 2 field-against-child-scope resolution |
| `parser.c:774` | `Identifier.resolved = NULL` | Initialization at parse time |

**Sema writes (the ones that matter for HIR):**

| Site | Field | Why sema does it | HIR migration |
| --- | --- | --- | --- |
| `checker.c:505` | `expr->field.field.resolved` in `expr_Field` infer path | The resolver couldn't pre-link this field because the object's type is only known at sema time (e.g. `header^.size`). Sema fills it in after `sema_infer_expr` on the object yields a `child_scope`-bearing type. | AST→HIR lowering does the same lookup and writes the result to the HIR instruction (not back to the AST). The mutation goes away. |
| `checker.c:1628` | `expr->enum_ref_expr.name.resolved` in `sema_check_expr` | The variant lookup needs the expected enum type's `child_scope`. Resolver runs without expected types, so this happens at sema. | Same — HIR lowering resolves the variant against the expected type and writes the variant decl into the HIR enum-ref node. |

**Findings:**

- Only **two** sema-side AST mutations exist. Both are well-scoped
  resolution gap-fills with explanatory comments at the call site.
- `const_eval.c` and `instantiate.c` do **not** mutate the AST — verified
  by grep. They read `Identifier.resolved` and other resolver-set fields
  but never write back.
- The AST is functionally read-only after sema, modulo these two writes.

**Action:** keep as-is. Moving the two writes to side-tables now would
add complexity without payoff — they're contained, documented, and HIR
lowering will absorb them naturally. Recorded here so the HIR migration
checklist explicitly accounts for both sites.

The AST-is-read-only contract is much closer to honored than the audit's
"likely candidates" list suggested. The plan listed five potential
mutation points; only two are real.

### Pass 5 — Fact-recording

#### Recording sites

`sema_record_fact` is called from 7 sites, plus `sema_record_call_value`
once. The two entry points are documented in
[sema_internal.h](../src/sema/sema_internal.h):

- `sema_infer_expr` records **unconditionally** at the end of its
  per-kind switch ([checker.c:1498](../src/sema/checker.c#L1498)) —
  every expr kind that flows through the infer path produces a fact.
- `sema_check_expr` records in **each branch** that diverts away from
  the infer-path fallthrough (Block, If, Product-with-struct,
  TYPE_TYPE, EnumRef-with-enum). The fallthrough delegates to
  `sema_infer_expr` which records via #1.

#### Coverage analysis

Walked every check-side code path. Result:

- All success paths record. ✓
- The infer-path catch-all records for every expression kind. ✓
- `const_eval.c` and `instantiate.c` do not record facts (correct —
  they don't claim to type-check anything; sema does that).

**Two error paths missed**, both in `sema_check_expr`/`check_block_expr`
where check-failure returned `false` without recording:

| Site | Gap | Action |
| --- | --- | --- |
| `check_block_expr` empty-block type-mismatch (was line 250) | `report_type_mismatch` then `return false` — no fact recorded for the empty block. Downstream consumers (HIR lowering, dump_tyck) would skip it entirely. | **Added** `sema_record_fact(s, expr, s->error_type, SEM_VALUE, 0)` before the failure return so every expr in the body has a fact even when checking failed. |
| `sema_check_expr` EnumRef variant-not-found (was line 1638) | Same pattern: error reported, returned `false`, no fact for the EnumRef expression. | **Added** the same error-fact write before the failure return. |

The convention now: **every expression sema visits — success or failure
— records a fact**. Error paths record `s->error_type`.

For HIR migration this matters because lowering needs to find a fact
for every expr it encounters. Missing facts would force HIR lowering
to either skip those nodes (silently dropping errored exprs) or do its
own re-inference (duplicate work). Both are worse than the small extra
write here.

Tests: 76 passing throughout.

### Pass 6 — Memory ownership

#### Big-struct allocation map

Every "long-lived" struct in the type/sema layer is **arena-allocated**.
Verified for all of:

| Struct | Site | Arena |
| --- | --- | --- |
| `Decl`, `Scope`, `Module` | name_resolution.c | `Resolver.arena` (`= compiler->arena`) |
| `Type` (named, optional, slice/ptr/array variants) | type.c | `Sema.arena` |
| `SemaDeclInfo` | sema.c | `Sema.arena` |
| `CheckedBody` | sema.c | `Sema.arena` |
| `EffectSig`, `EffectSet`, `EvidenceVector` | effects/effect_solver/evidence | `Sema.arena` |
| `Instantiation` | instantiate.c | `Sema.arena` |
| `ConstStruct`, `ConstArray`, `ComptimeEnv`, `ComptimeCell`, `ComptimeArgTuple`, `ComptimeCallCacheEntry` | const_eval.c | `Sema.arena` |

No big-struct uses heap. Ownership story is uniform: the compiler's
arenas own everything, freed in bulk at compiler teardown.

#### Heap allocations (legitimate)

| Site | What | Why heap |
| --- | --- | --- |
| `module_loader.c:155` (`realpath(filepath, NULL)`) | canonical file path | POSIX `realpath` returns malloc memory; freed at name_resolution.c:266 + 273. |
| `module_loader.c:70` (`malloc(file_size + 1)`) | file source buffer | Long-lived (until compiler teardown via SourceMap), freed in `sourcemap_free_sources`. |
| `stringpool.c` (`malloc`/`realloc`/`free`) | pool slot table + data buffer | StringPool is a heap-backed grow-as-needed structure by design. |

All justifiable; no leaks visible in the pairing.

#### Dead heap-allocating code (deleted)

Three dead functions wired heap allocation into the codebase. None
were called from anywhere (`src/` or `tools/`):

| Function | File | Issue | Action |
| --- | --- | --- | --- |
| `normalizer(Vec* tokens, StringPool* pool)` | layout.c:270 | Heap-allocated `output` Vec was never freed (returned to caller, no caller). Heap-allocated `frames` Vec was malloc'd inside `normalizer_new` and freed here. Whole function unreferenced. | **Deleted.** |
| `normalizer_new(Vec* tokens)` | layout.c:28 | Wrapper that called `normalizer_new_in(tokens, NULL)`, taking the dead heap fallback path. | **Deleted** (function + its declaration in layout.h). |
| `normalizer_new_in`'s `else` branch (heap fallback) | layout.c:12-18 | Mallocked two `Vec*`s when arena was NULL. Now unreachable since the only caller (`normalizer_in`) always passes `compiler->pass_arena`. | **Deleted.** Now requires non-NULL arena. Removed the `<stdlib.h>` include (no more malloc/free). |
| `parser_new(Vec* tokens, StringPool* pool)` | parser.c:445 | `Arena* a = malloc(sizeof(Arena))` then `arena_init(a, ...)` — leaks `a` (no `free(a)` anywhere, the arena allocation isn't tracked). Function unreferenced. | **Deleted.** |
| `parser_new_in` | parser.c:441 | Wrapper around `parser_new_in_with_diags(.., NULL)`. Unreferenced. | **Deleted** (consolidating to one entry point). |

#### Findings

- Big-struct ownership is fully consistent — arena throughout.
- Three dead heap-using functions removed; the only one with an actual
  leak (`parser_new`) is gone.
- HIR will inherit a clean arena story; no per-struct ownership rules
  to design.

Tests: 76 passing throughout.
