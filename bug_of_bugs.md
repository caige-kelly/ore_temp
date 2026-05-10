# bug_of_bugs.md — Latent bugs, architectural debt, and lessons from PR 1 / PR 2

A persistent log of every concrete defect or load-bearing assumption surfaced
while implementing PR 1 (Foundation) and PR 2 (Comptime correctness). Distinct
from `cleanup.md`, which catalogs work to do; this catalogs **what we
discovered was already wrong** and the bug-class each finding belongs to.

Each entry has:
- **Where**: file path + line range when applicable.
- **Symptom**: how the bug manifested (test name if a harness scenario found it).
- **Root cause**: the actual mechanism.
- **Status**: `FIXED` (in which commit/PR) / `DEFERRED` / `OPEN` / `KNOWN-PARTIAL`.
- **Bug class** (for pattern-matching future cases).

Bug classes used throughout:
- **AST-dep gap**: a query reads AST-derived data without recording a dep on
  `query_module_ast`. Cache stays stale across re-parses.
- **Slot-state asymmetry**: ERROR / RUNNING / EMPTY paths handle revalidation
  differently than DONE, leading to lost recompute signals.
- **Hand-managed cache**: a side table that requires manual refresh on edits;
  easy to forget a field; not enforced.
- **Sentinel-on-cycle**: a query returns a "don't know" placeholder on cycle
  rather than recovering. Hides correctness issues.
- **Coarse fingerprint**: a slot's fingerprint loses too much information,
  silently masking real changes.

---

## Critical (currently broken on `main`, not caused by PR 1/2 work)

### B1 — `tools/test.sh` rewritten as slim smoke runner `[FIXED]`
- **Where (was)**: [tools/test.sh](tools/test.sh) — ~1400 lines of inline
  `.ore` test material plus C smoke-test builds. Header comment said
  `## TODO: fix. broken after sema refactor`.
- **Symptoms**:
  1. The `-fsanitize=address` + `-lasan` link recipe failed with
     `undefined symbol: ___asan_*` on macOS. Filed as B22 below
     (Zig-side issue, not ours).
  2. Five referenced sema source files (`src/sema/type.c`,
     `src/sema/query.c`, `src/sema/const_eval.c`, `src/sema/layout.c`,
     `src/sema/target.c`) were moved into subdirectories during the
     sema refactor. None resolved.
  3. Eight referenced example files
     (`examples/imports/import_simple.ore`, `examples/sema_skeleton.ore`,
     etc.) were pruned when `examples/` was reorganized to
     `examples/tests/` + `examples/allocator/`.
  4. CLI flags `--dump-sema` and `--dump-evidence` no longer exist.
  5. ~71 inline diagnostic-substring tests referenced features
     (effects, scoped handlers, `with`-blocks, evidence vectors)
     that aren't implemented in the current sema rebuild.
- **Status**: **FIXED**.
- **Class**: Build-config drift + accumulated dead test scaffolding.
- **Fix**: Replaced the 1400-line script with an ~80-line smoke
  runner that does what the rest of the suite doesn't:
  - Builds and runs `tools/arena_test.c` and `tools/hashmap_test.c`
    under UBSan (ASan is unavailable on macOS+zig — see B22).
  - Loops over `examples/tests/*.ore` and verifies exit codes match
    the naming convention (`*_errors.ore` must exit non-zero, all
    others must exit 0). Catches "fixture that used to fail now
    silently passes" and vice versa — neither
    `make test-invalidation` nor `make test-determinism` covers
    that property today.
  - The 71 inline diagnostic-substring checks were intentionally
    *not* ported. Specific error-message coverage is captured
    inside fixture comments (e.g.,
    `examples/tests/typed_const_bind_errors.ore` documents what
    each case should diagnose) and verified end-to-end by the
    determinism harness running the exact same fixtures across
    two runs.
- **Verification**: `make test` → 43 passed, 0 failed (2 C smoke +
  41 fixture exit-code checks). Runs cleanly inside `nix develop`
  on macOS aarch64.

### B22 — `zig cc -fsanitize=address` link failure on macOS
- **Where**: surfaces during `make test` when building C smoke tests
  with `zig cc -fsanitize=address` on aarch64-darwin.
- **Symptom**: `error: undefined symbol: ___asan_version_mismatch_check_v8`
  (and friends like `___asan_stack_malloc_1`). Adding `-shared-libsan`
  doesn't help — Zig still emits references to the static-link symbols.
- **Root cause**: Upstream Zig issue. Zig's bundled compiler-rt for
  darwin doesn't ship the version-check / stack-instrumentation
  symbols the AddressSanitizer runtime expects. Discussed in several
  Zig GitHub issues; not yet resolved as of zig 0.16.0.
- **Status**: **OPEN**, low priority. Worked around by using UBSan
  (`-fsanitize=undefined`) in `make test`.
- **Class**: External toolchain bug (not Ore code).
- **Action**: Periodically re-test after Zig version bumps. When
  upstream fixes it, swap `-fsanitize=undefined` back to
  `-fsanitize=address` in [Makefile](Makefile)'s `TEST_CFLAGS`.
  Alternatively, switch the smoke-test build to use system clang
  (which ships a working ASan runtime on macOS) — but that defeats
  the "single toolchain" property of the Nix shell.

### B2 — Type checking only fires on `--dump-tyck` `[FIXED in PR 3 Layer 0]`
- **Where**: [src/main.c:141](src/main.c#L141). Only `dump_tyck` (and indirectly
  the determinism harness which always passes `--dump-tyck`) iterated top-level
  decls and called `query_type_of_def`. The production-mode driver
  (`./ore file.ore`) did not type-check.
- **Symptom**: `m_overflow : u8 = 1024` compiled cleanly with `./ore file.ore`
  exit 0. Only `./ore --dump-tyck file.ore` surfaced the error.
- **Root cause**: The driver ran `query_module_def_map` +
  `scope_index_build_module` and stopped. Typecheck was gated by the dump flag.
- **Fix**: Added `sema_check_module(s, mid)` in `src/sema/type/checker.c`;
  driver calls it unconditionally after `scope_index_build_module`. Dumpers
  stay orthogonal — they consume results, not produce them.
- **Latent failures surfaced** (each was a fixture bug, not a typechecker bug):
  - `examples/tests/typed_numbers.ore` had `overflow_u8`, `neg_u8`,
    `overflow_i8`, `overflow_f32` decls with values that overflow their target
    types. Removed; the same shapes are tested via `tools/test.sh`'s ad-hoc
    `run_failure_contains` fixtures.
  - `examples/tests/typed_unary_ptr.ore` had `p : ^i32 : &x` — `::` against a
    runtime value (`&x` is the address of a local var). Changed to `:=`.
  - `examples/tests/enums.ore` had `is_painted :: paint(.Blue)` — `::` against
    a runtime fn call. Wrapped in fn shapes (`is_painted :: fn() -> bool`).
- **Status**: **FIXED**.
- **Class**: Driver flow.

---

## High (load-bearing, fragile, will break)

### B3 — `node_to_expr` is monotonically growing across re-parses `[CORRECTNESS VERIFIED — memory bloat only]`
- **Where**: [src/sema/resolve/scope_index.c:55-65](src/sema/resolve/scope_index.c#L55-L65)
  — `record_node_expr` does `hashmap_put` keyed by `NodeId`. Same NodeId
  from a later parse overwrites the prior `Expr*`. NodeIds present in
  the prior parse but absent in the new one stay as orphaned entries.
- **Original concern**: Stale Expr* pointers might be dereferenced
  through `def_origin` for a stale `origin_id`.
- **Outcome of test-first investigation (T16, T17, T18)**: Not a
  correctness bug. Every call site that walks the map via NodeId uses
  the CURRENT revision's NodeId (set at the corresponding `def_origin`
  / Expr-keyed cache entry). Orphans accumulate but are never read.
  The PR 1 `def_origin` migration + DefMapEntry-reuse path together
  maintain the invariant.
- **Remaining concern**: Memory bloat. ~few hundred bytes per orphan
  entry; 1000 edit cycles in an LSP session leaks ~200KB. All in the
  arena, which never shrinks anyway — net effect is one fixed cost.
- **Status**: **NOT A CORRECTNESS BUG; LATENT MEMORY BLOAT**. Tests
  T16/T17/T18 in `tools/sema_invalidation_test.c` lock in the
  current correctness; future regressions will fail visibly.
- **Class**: Hand-managed cache (memory only).
- **Action (long-term)**: When Ore grows a real long-running LSP
  shell, generational arenas (rust-analyzer's per-revision model) is
  the right answer, not piecewise-clearing individual hashmaps.

### B4 — `top_level_index` recompute leaks `DefMapEntry` for removed names `[CORRECTNESS VERIFIED — memory bloat only]`
- **Where**: [src/sema/modules/def_map.c:127-141](src/sema/modules/def_map.c#L127-L141)
  — `get_or_create_entry` keeps every `DefMapEntry` alive in
  `m->def_map_entries`. When source A defines `foo` and source B
  removes it, foo's entry stays in the hashmap with `def = D`.
- **Original concern**: Stale DefIds in `internal_scope` could
  collide with re-added names; "duplicate-name scenario causes
  scope corruption."
- **Outcome of test-first investigation (T16, T18)**: Not a
  correctness bug. PR 1's DefMapEntry-reuse path REUSES the same
  DefId across rename → re-add cycles. Source A:`foo:1` → B:`bar:2`
  → C:`foo:3` returns the SAME DefId for `foo` in revisions A and
  C, with `DefInfo` refreshed to point at revision C's AST. The
  scope's name_index entry stays consistent (same DefId both times)
  and `scope_define_def` is never called twice for the same name.
- **Remaining concern**: Memory bloat — orphaned `DefMapEntry`
  hashmap entries for names that get permanently removed
  (renamed-and-never-re-added). All in arena.
- **Status**: **NOT A CORRECTNESS BUG; LATENT MEMORY BLOAT**.
  Tests T16/T18 lock in the behavior.
- **Class**: Hand-managed cache (memory only).
- **Action**: Same as B3 — generational arena strategy when LSP
  scale demands it.

### B5 — `defs_table` and `scope.defs` grow monotonically `[CORRECTNESS VERIFIED — memory bloat only]`
- **Where**: [src/sema/modules/def_map.c:254](src/sema/modules/def_map.c#L254)
  (`def_create`) — appends to `s->defs_table` with no shrink.
- **Original concern**: `sema_def_count(s)` returns inflated numbers
  in long sessions; iteration paths might see ghost entries.
- **Outcome of test-first investigation (T17)**: 20 alternating
  add/remove cycles produce no incorrect behavior. Reason same as
  B4: PR 1's reuse path means re-added names recover their original
  DefId (the slot caches by name → DefId), so the `defs_table` grows
  by O(distinct-names-ever-defined), not by O(edits). For typical
  source files that's bounded by program complexity.
- **Remaining concern**: True orphans (a name that gets renamed and
  never comes back) leave their DefId behind. Same memory profile as
  B4.
- **Status**: **NOT A CORRECTNESS BUG; LATENT MEMORY BLOAT**.
  T17 covers the multi-edit stress case.
- **Class**: Hand-managed cache (memory only).
- **Action**: Generational arena strategy at LSP scale.

### B6 — two-namespace fallback in `query_resolve_ref` consolidated `[FIXED]`
- **Where (was)**: [src/sema/resolve/resolve.c](src/sema/resolve/resolve.c) +
  call sites in `expr_check.c` and `const_eval.c`. Three callers (and
  more transitively) tried `NS_VALUE` first then `NS_TYPE` on miss —
  two GUARD evaluations, two slot lookups, two `record_dep_on_parent`
  calls per Ident in the fallback case.
- **Status**: **FIXED**.
- **Class**: Slot-key over-granularity for fallback callers.
- **Fix**: Added `NS_VALUE_OR_TYPE` to the `Namespace` enum
  ([src/sema/scope/scope.h](src/sema/scope/scope.h)) — semantically
  "either value or type is acceptable here." Inside
  `query_resolve_ref`'s body the resolver does prefer-value-fall-back-
  to-type within a single slot (preserving existing semantics — the
  fallback walks the chain twice, but inside one slot, with one dep
  recording on the parent). `ns_match` accepts either value or type
  defs when `want == NS_VALUE_OR_TYPE`. Three call sites migrated:
  - `expr_check.c:288-290` (was NS_VALUE → NS_TYPE on miss)
  - `const_eval.c:280-282` (was NS_VALUE → NS_TYPE on miss in
    `query_is_comptime`)
  - `const_eval.c:381` (was NS_VALUE only; converted for slot-sharing
    with the two above — semantically identical because the
    subsequent `bind_Const`/`bind.value` checks filter type defs out
    just like a missed NS_VALUE lookup would)
- **Effect**: For Idents that fall back to NS_TYPE (struct-construction
  syntax `Point{...}`, `@sizeOf(T)` in expr position, etc.), slot count
  drops from 2 to 1, dep recordings drop from 2 to 1, GUARD evaluations
  drop from 2 to 1. For Idents that resolve cleanly via NS_VALUE on
  first try (the common path), behavior is unchanged. Single-namespace
  callers (effect_ops `NS_EFFECT`, type/checker `NS_TYPE`,
  index/position `NS_VALUE`) keep their existing slot semantics.
- **Verification**: 21/21 invalidation in both build modes, 41/41
  determinism, 41/41 production fixtures both modes. `dump.c`'s
  `ns_name` switch updated to recognize the new variant.

### B7 — `query_const_eval` fingerprint hashes the entire `ConstValue` struct `[FIXED in PR 3 Layer 0]`
- **Where**: [src/sema/eval/const_eval.c](src/sema/eval/const_eval.c) — the
  fingerprint-stamping site after the dispatch switch.
  `query_fingerprint_from_bytes(&result, sizeof(result))` included the
  union's inactive variant bytes (uninitialized for any kind != the active
  one).
- **Symptom**: Reads uninitialized memory. Compilers usually zero-extend
  small unions so this was probably stable in practice, but it's UB and
  MSan / ASan with `-fsanitize=memory` would flag it.
- **Root cause**: Naive blob hash on a tagged union.
- **Fix**: Hash `(kind, active-variant-bytes)` only — switch on `kind` and
  feed the right field via `query_fingerprint_combine`. Floats are
  bit-cast through `memcpy` to a `uint64_t` to avoid strict-aliasing
  pitfalls. Same shape extends naturally to any future ConstValue variant
  (struct payloads in PR 2.5).
- **Status**: **FIXED**.
- **Class**: UB.

### B8 — `record_ast_dep_for_expr` duplicates `sig_record_ast_dep`
- **Where**: [src/sema/eval/const_eval.c](src/sema/eval/const_eval.c) (PR 2)
  and [src/sema/type/decl_data.c](src/sema/type/decl_data.c) (PR 1) each have
  their own AST-dep-recording helper. Both call `query_module_ast` from the
  active query frame, just from different "I have a DefId" vs. "I have an
  Expr* / span" entry points.
- **Symptom**: Two helpers, two implementations, two places to forget. Future
  AST-reading queries will need a third copy.
- **Root cause**: Helper utility never extracted.
- **Status**: **OPEN**.
- **Class**: Duplication.
- **Action**: Extract to `src/sema/query/ast_dep.{h,c}` with two entry points
  (`record_ast_dep_for_def(s, def)` and `record_ast_dep_for_span(s, span)`).
  Migrate both call sites.

### B9 — `query_def_for_name` slot fingerprint stays at `FINGERPRINT_NONE`
- **Where**: [src/sema/modules/def_map.c:237, :273](src/sema/modules/def_map.c)
  — both `sema_query_succeed` calls succeed without setting a fingerprint.
- **Symptom**: Dependents on `def_for_name` can't early-cut by fingerprint
  comparison; they always re-walk. PR 1 worked around this by having signature
  queries depend on `query_module_ast` directly instead of on `def_for_name`.
- **Root cause**: The slot's fingerprint should encode (DefId, semantic_kind,
  vis) so e.g. a `pub` toggle invalidates dependents while a body-only edit
  to the same name doesn't.
- **Status**: **OPEN**, papered over by AST-dep workaround.
- **Class**: Coarse fingerprint.
- **Action**: After the reuse path / first-time create, compute
  `fp = combine(DefId.idx, sem, vis)` and call `query_slot_set_fingerprint`.

---

## Medium (won't bite soon, but worth tracking)

### B10 — `DefInfo` is a hand-managed cache with implicit refresh contract
- **Where**: [src/sema/modules/def_map.c:225-239](src/sema/modules/def_map.c#L225-L239)
  — the reuse path manually refreshes `semantic_kind`, `span`, `origin_id`,
  `origin`, `vis`. Easy to forget a field. Adding a new field to `DefInfo`
  silently breaks invalidation if you don't also touch the reuse path.
- **Symptom**: Forgotten field stays at source-A value across an edit. Test
  T6 (rename) caught a version of this for `vis` during PR 1 development.
- **Root cause**: The "thin identity" design split per-kind data into side
  tables (FnSignature, ParamLocator, etc.) but `DefInfo` itself still has
  cache-shaped fields (`origin`, `origin_id`, `child_scope`).
- **Status**: **OPEN**.
- **Class**: Hand-managed cache.
- **Action**: Long-term, treat `DefInfo` as truly immutable identity (kind,
  name_id, owner_scope only) and move *every* re-parse-affected field into a
  side query — `query_def_origin(def)` returns the Expr*/NodeId/span, with
  its own slot dep-graph integrated. PR 4+ work.

### B11 — `def_origin` indirection silently depends on `scope_index_build_module` running
- **Where**: [src/sema/ids/ids.c:73-83](src/sema/ids/ids.c#L73-L83) — falls
  back to the raw `di->origin` if `node_to_expr` doesn't contain `origin_id`.
- **Symptom**: A query that runs *before* `scope_index_build_module` has
  populated `node_to_expr` for the current revision will get the *prior*
  parse's Expr* (stale). Today nothing triggers this because the driver
  always runs scope_index after def_map, but the contract isn't enforced.
- **Root cause**: Two-phase population without a barrier.
- **Status**: **OPEN**, latent.
- **Class**: Implicit ordering contract.
- **Action**: Either make `def_origin` itself trigger scope_index population
  for its module (lazy), or assert that `s->node_to_expr` has been built for
  the current revision before any def_origin call.

### B12 — `scope_define_def` may collide on rename re-add `[NOT REPRODUCIBLE — fixed by PR 1's reuse path]`
- **Original concern**: Edit `old :: 1` → `new :: 2` → `old :: 3`
  and `old`'s third-revision DefId silently fails to insert because
  the orphan from revision 1 is still in scope.
- **Outcome of test-first investigation (T16)**: The bug as
  described doesn't reproduce. T16 drives exactly that source
  sequence and asserts:
    1. `old` resolves before the rename (✓)
    2. `old` is unresolvable after rename (✓)
    3. `old` resolves after re-add with the correct folded value (3,
       not the stale 1) (✓)
  The reason it works: PR 1's DefMapEntry-reuse path returns the
  SAME DefId across the rename → re-add cycle. `scope_define_def`
  isn't called a second time — the existing scope name_index entry
  for `old` (still pointing at the same DefId) remains valid. The
  `DefInfo` reuse path (def_map.c:225-239) refreshes the
  AST-pointing fields (origin, origin_id, span, semantic_kind, vis)
  to reflect the new revision.
- **Status**: **FIXED — proven by T16**. The fix landed in PR 1
  (the DefMapEntry reuse path), but B12 was filed before it was
  understood that the fix already covered this case. T16 makes
  that property explicit.
- **Class**: Accumulator without unrolling (turned out the
  unrolling wasn't needed — same identity is reused).

### B13 — Cycle returns sentinel, not fixpoint
- **Where**: All `SEMA_QUERY_GUARD` `on_cycle` arguments —
  [src/sema/eval/const_eval.c](src/sema/eval/const_eval.c),
  [src/sema/type/decl_data.c](src/sema/type/decl_data.c),
  [src/sema/type/checker.c](src/sema/type/checker.c). Each returns
  `DEF_ID_INVALID` / `CONST_NONE` / `s->error_type` / `false`.
- **Symptom**: `A :: A + 1` doesn't error with "circular definition"; it
  silently produces CONST_NONE and the bind type-checks (depending on path).
  Today `def_for_name` does emit "circular definition of 'X'" via
  `self_referential_value_file` test, so module-level cycles surface, but
  expression-level cycles in const_eval don't.
- **Root cause**: Salsa supports `CycleRecoveryStrategy::Fixpoint` for self-
  referential type inference. We chose sentinel-on-cycle because identity-only
  TY_STRUCT sidesteps the immediate need; for E.5 (generics) we'll want real
  fixpoint.
- **Status**: **DEFERRED** to post-E.4 (cleanup.md #17).
- **Class**: Sentinel-on-cycle.
- **Action**: When E.5 work starts, integrate fixpoint cycle recovery via
  rust-analyzer / Salsa's pattern.

### B14 — No telemetry on the query system
- **Where**: Whole `src/sema/query/` directory.
- **Symptom**: Can't answer "how many queries ran cold-start? what's the
  cache hit rate? which slots got evicted?" Performance debugging is blind.
- **Root cause**: No counters / dump_query_stats / per-kind stats.
- **Status**: **OPEN**.
- **Class**: Observability gap.
- **Action**: Add per-QueryKind counters (begin / cached / compute / cycle /
  error) behind `#ifdef ORE_DEBUG_QUERIES` (the same flag #16 will use).
  Dump on `--dump-query-stats`.

---

## Known partials filed during PR 3 Layer 1

These are deliberately-deferred features surfaced while implementing
the type-system layer. None are correctness regressions; each would
extend an already-working facility.

### B15 — `nil → ?T` literal coercion missing `[FIXED in PR 3.5]`
- **Where (was)**: `Sema.nil_type` field on
  [src/sema/sema.h](src/sema/sema.h) was declared but never initialized.
  `lit_Nil` typed as `s->nil_type` in
  [src/sema/type/expr_check.c:276](src/sema/type/expr_check.c#L276)
  which was NULL → fell back to `s->error_type`. So `nil` was unusable.
- **Symptom (was)**: `let p: ?^Header = nil` errored; user had to
  construct the optional value through other means.
- **Root cause (was)**: No `TY_NIL` primitive, no typed-nil literal
  handling, no `nil → ?T` coerce rule, no `nil → ^T` (any pointer)
  coerce rule.
- **Fix**:
  1. Added `TY_NIL` to the type kind enum
     ([src/sema/type/type.h](src/sema/type/type.h)).
  2. `s->nil_type = PRIM(TY_NIL, "nil")` in `sema_types_init`
     ([src/sema/type/type.c](src/sema/type/type.c)).
  3. `type_name` + display rendering both produce `"nil"`.
  4. Coerce rule in `coerce_check`
     ([src/sema/type/coerce.c](src/sema/type/coerce.c)): if
     `from->kind == TY_NIL` and `to->kind` is one of `TY_OPTIONAL`,
     `TY_PTR`, `TY_MANY_PTR`, `TY_SLICE`, accept. Anything else
     (scalars, structs, enums, bool) falls through to the existing
     "expected X, got Y" diagnostic.
- **Tests**:
  - `examples/tests/optional_nil.ore` — positive coverage across
    `?T`, `^T`, `^const T`, `[^]T`, `[]const T`, `?^T`, and a
    locally-bound `p : ^i32 = nil`.
  - `examples/tests/optional_nil_errors.ore` — `nil → i32`, `u8`,
    `bool`, `f64`, and a user-struct-typed parameter all produce
    precise per-call-site diagnostics.
- **Design choice**: permissive (Ore today accepts `^T = nil` as a
  typed null pointer). Strict version (Zig-style: only `?*T = null`,
  no `*T = null`) is a deliberate language-design decision that
  could tighten this later. Filed alongside R4 as a future
  language-discipline question.
- **Status**: **FIXED**.
- **Class**: Incomplete lattice.

### B16 — `?T` unwrapping operations missing (`?` postfix, `orelse`) `[FIXED in PR 3.5]`
- **Where (was)**: AST had `unary_DeNil` and lexer/parser had `OrElse`
  wired, but sema's `type_of_unary` left DeNil in the default branch
  (returning error_type) and `type_of_bin` didn't dispatch `OrElse`.
- **Symptom (was)**: User could declare `?T` parameters/returns (PR 3
  #6) but couldn't extract a `T` from them. Optionals were write-only.
- **Root cause (was)**: PR 3 #6 was scoped to type-system semantics
  only. Operations are op-shaped (require expr-checker support) and
  were explicitly deferred.
- **Fix**:
  1. `unary_DeNil` case in `type_of_unary`
     ([src/sema/type/expr_check.c](src/sema/type/expr_check.c)):
     reject if operand isn't `TY_OPTIONAL`; otherwise return
     `t->optional.elem`.
  2. `bin_orelse_result` helper + `OrElse` case in `type_of_bin`:
     reject if left isn't `TY_OPTIONAL`; otherwise coerce right to
     the unwrapped element type and return that. The `noreturn`
     coerce rule (PR 3 #5) makes the early-return idiom
     `o orelse return ...` work transparently.
- **Tests**:
  - `examples/tests/optional_unwrap.ore` — postfix `?` on optionals,
    `orelse` with literal/comptime/optional-pointer/early-return
    fallbacks, postfix on fn-call results, chained `orelse`.
  - `examples/tests/optional_unwrap_errors.ore` — postfix on
    non-optional / on pointer, `orelse` on non-optional left, type-
    mismatched fallback, overflowing comptime fallback.
- **Runtime semantics deferred**: postfix `x?` is a panic-on-nil
  unwrap; that's codegen's responsibility (we don't emit code yet).
  At the type level we just yield the inner type and let the
  eventual codegen emit the unwrap-or-panic logic.
- **Status**: **FIXED**.
- **Class**: Incomplete operator surface.

### B17 — Anonymous user-defined types in fn-type-position params `[FIXED via Fn/fn split]`
- **Where (was)**: [src/sema/type/checker.c:120-145](src/sema/type/checker.c#L120-L145) —
  the lambda case in `resolve_type_expr` accepted bare names only via
  `type_for_primitive_name`. User types (struct/enum) needed the
  annotated form.
- **Symptom (was)**: `fn(Point, Color) -> i32` errored with "bare type
  name must be a primitive"; user had to write
  `fn(p: Point, c: Color) -> i32`.
- **Root cause (was)**: The parser parsed `fn(P)` as a Param with
  `name=P, type_ann=NULL` (Param required an identifier first). In type
  position, there was no syntactic disambiguation between "param named
  P, no type" and "anonymous param of type P".
- **Fix**: Split capital `Fn` (type) from lowercase `fn` (value/lambda).
  `Fn(P, Q) -> R` is unambiguously type-position; each comma-separated
  entry parses as a full type expression directly into a Vec<Expr*>.
  No Param structs, no name vs. type ambiguity, no primitive-name
  heuristic. See F15 below for the detailed list of code that was
  deleted.
- **Status**: **FIXED**.
- **Class**: Parser limitation (resolved at the language-design level).

### B18 — `[^]T - [^]T` pointer difference + pointer comparisons `[FIXED]`
- **Where (was)**: [src/sema/type/expr_check.c](src/sema/type/expr_check.c) —
  the many-pointer arith branch handled `[^]T +/- int` only. Two
  many-pointers subtracted errored out, and the comparison path
  rejected pointer-pointer comparisons entirely. Iterator-style code
  (`while cur != end { ... }`) didn't typecheck.
- **Status**: **FIXED**.
- **Class**: Incomplete operator surface.
- **Design (Zig as reference)**:
  - `[^]T - [^]T → usize`: matches `analyzeArithmetic`
    ([zig/src/Sema.zig:14902-14997](zig/src/Sema.zig#L14902-L14997)).
    Result is unsigned because signed would require nailing down
    "lhs >= rhs is required" semantics; unsigned-with-overflow on
    `lhs < rhs` matches Zig's policy.
  - Equality `==` / `!=` between pointers: allowed when both have the
    same interned pointer type (same elem AND same constness — `l == r`
    pointer-comparison covers both, since types are interned).
  - Ordering `<` / `<=` / `>` / `>=` between pointers: **rejected**.
    Matches Zig's `isSelfComparable`
    ([zig/src/Type.zig:340](zig/src/Type.zig#L340)) — only C pointers
    get ordering; regular pointers / many-pointers don't, because
    ordering is well-defined only when both pointers come from the
    same allocation (which can't be verified statically). Iterator
    code uses `cur != end` against a sentinel, which only needs
    equality.
  - Single-pointer subtraction `^T - ^T`: rejected. Zig allows it for
    `*T` but flags the semantics as confusing
    ([Sema.zig:14928-14938](zig/src/Sema.zig#L14928-L14938)). Ore
    restricts subtraction to many-pointers.
- **Fix**:
  - `bin_arith_result`'s Minus branch: when both sides are
    `TY_MANY_PTR` and `l == r` (interned-equal type), check elem-type
    layout via `query_layout_of_type` for known non-zero size, then
    return `s->usize_type`. Diagnostic on type mismatch or zero-size
    elem.
  - `bin_cmp_result`: new `else if` branch for pointer-comparison.
    Accepts `EqualEqual` / `BangEqual` when both sides are interned-
    equal pointer types (`TY_PTR` or `TY_MANY_PTR`). All ordering ops
    fall through to the existing "incompatible types" error.
- **Tests**: `examples/tests/many_ptr_arith.ore` gains 5 positive
  cases (ptr_diff, ptr_diff_const, ptr_eq, ptr_neq, single_ptr_eq);
  `many_ptr_arith_errors.ore` gains 6 error cases (`+ ptr`, elem-type
  mismatch, constness mismatch, `<`, `>=`, single-ptr subtraction).
  Both fixtures pass under default and debug builds.

### B19 — `coerce`'s `emit` flag refactored away `[FIXED]`
- **Where (was)**: [src/sema/type/coerce.c](src/sema/type/coerce.c) —
  `coerce_check(emit)` threaded a boolean through every branch to
  decide whether diagnostics fire. Speculative callers (the
  optional-lift recursion) passed `emit=false`; the public `coerce`
  wrapper passed `emit=true`. Diagnostic emission was entangled with
  structural decision-making.
- **Status**: **FIXED**.
- **Class**: Code smell.
- **Fix**: Split into two layers:
  - `coerce_structural(from, to, value) → CoerceResult` — pure
    structural+range predicate. No `Sema*` parameter, no diagnostics,
    no mutation. Returns `COERCE_OK` / `COERCE_FAIL_TYPE` /
    `COERCE_FAIL_RANGE`. Speculative recursion (optional-lift) calls
    itself directly, no flag-threading.
  - `coerce(s, from, to, value, span) → bool` — thin emitting wrapper.
    Calls the predicate, switches on the result to pick the right
    diagnostic shape (range error gets value+bounds spelled; type
    error gets the "expected X, got Y" form), returns success bool.
    `emit_range_error` is a small helper that re-derives `lo`/`hi`
    via a second `fits_in` call — keeps the predicate Sema-free at
    the cost of one cheap re-check on the failure path.
- **Effect**: The next speculative coerce rule (variance, generics,
  whatever) just calls `coerce_structural` directly — no flag to
  thread, no emission concerns. Public `coerce()` API unchanged.
- **Verification**: 21/21 invalidation in both build modes, 41/41
  determinism, 41/41 production fixtures both modes. Diagnostic
  shapes for range / type-mismatch errors are byte-identical to
  pre-refactor (verified via determinism harness, which compares
  per-fixture stdout/stderr across two runs).

### B21 — `query_node_to_decl` / `query_scope_for_node` driver-tracked → slot-tracked `[FIXED]`
- **Where (was)**:
  - [src/sema/resolve/scope_index.c](src/sema/resolve/scope_index.c) —
    `query_node_to_decl(s, NodeId)` and `query_scope_for_node(s, NodeId)`
    were non-GUARD'd lookup functions reading `s->node_to_decl` and
    `res->node_to_scope` directly.
- **Symptom (was)**: Both worked in practice but only because
  `scope_index_build_module` ran after every re-parse before any
  consumer query read them. The contract was enforced by *driver
  pipeline ordering*, not by the dep graph. Consumers
  (`query_resolve_ref`, `query_type_of_expr`) recorded no dep on the
  producer — refactoring the pipeline order could have silently
  broken correctness.
- **Why not just SEMA_READ_UNTRACKED**: tried during #16
  implementation; forced RECOMPUTE on every transitive caller
  (resolve_ref → type_of_expr → const_eval), breaking T15's
  no-op-recompute property. The semantic mismatch: untracked means
  "could change without anyone knowing"; the real situation was
  "guaranteed-fresh by driver pipeline." Different concept.
- **Status**: **FIXED**.
- **Class**: Driver-ordering contract → dep graph contract.
- **Fix**: Promoted `query_node_to_decl_index(mid)` to a real
  slot-cached per-module query (slot lives on `ModuleInfo` as
  `node_to_decl_index_query`, kind `QUERY_NODE_TO_DECL`, dispatched
  in `sema_locate_slot`). Body records deps on `query_module_ast`
  and `query_top_level_index`, walks every top-level decl, and
  populates `s->node_to_decl`. Fingerprint = (count, sum of DefIds)
  so body-only edits don't shift it but add/remove/rename does.
  - **API change**: `query_node_to_decl(s, NodeId)` →
    `query_node_to_decl(s, struct Expr *)`. The Expr provides the
    span needed to resolve owning module via `module_for_span`,
    which is what lets the lookup record the proper dep.
    `query_scope_for_node` got the same treatment for symmetry and
    because it routes through `query_node_to_decl`.
  - **Caller updates**: 2 sites — `query_resolve_ref` in resolve.c
    and `type_of_return` in expr_check.c. Both already had `Expr*`
    in hand.
- **Verification**: `node_to_decl` slot appears in `--dump-query-stats`
  with stable counts (cold-drive: 30 begin / 29 cached / 1 compute
  for a 26-link comptime chain — populator runs once, every
  subsequent consumer gets cache). 21/21 invalidation in both build
  modes, 41/41 determinism, 41/41 production fixtures both modes.
- **Discovered**: during #16 migration audit, when looking for
  legitimate `SEMA_READ_UNTRACKED` sites and realizing these were
  tempting candidates that would be wrong to wrap.

### B20 — `query_top_level_index` should succeed-with-NULL, not fail, on absent AST `[FIXED]`
- **Where**: [src/sema/modules/def_map.c:75-79](src/sema/modules/def_map.c#L75-L79) —
  `query_top_level_index` enters its GUARD, calls `query_module_ast`,
  sees NULL, and calls `sema_query_fail`. The slot lands in ERROR
  state — showed up in `--dump-query-stats` as 1 error per
  primitives_init even though nothing actually went wrong.
- **Symptom**: Surfaced by B14's telemetry — a passing fixture had 1
  `error` count for `top_level_index`. Operationally harmless
  (callers handle NULL correctly), but architecturally wrong: ERROR
  is reserved for "this query genuinely failed," and "no source =
  empty top-level index" isn't a failure.
- **Root cause**: Used `sema_query_fail` for what Salsa models as a
  successful memo with `Option<Output> = None`
  ([salsa/src/function/memo.rs:85-96](salsa/src/function/memo.rs#L85-L96)).
  Salsa has no separate ERROR state — failure is part of the value
  type, and "no input" is just a `None`-valued successful memo.
- **Considered scope expansion (and rejected)**: `query_module_ast`'s
  early bypass at [modules.c:50-51](src/sema/modules/modules.c#L50-L51)
  *looked* like the same antipattern but turns out to be structurally
  necessary — its slot lives on `InputInfo*`, and primitives have
  `m->input == INPUT_ID_INVALID` (no `InputInfo`), so there's no
  slot to address. The bypass is correct.
- **Status**: **FIXED**.
- **Class**: Sentinel-NULL antipattern (narrow scope).
- **Fix**: Set `m->top_level_index = NULL`, fingerprint with
  `query_fingerprint_from_u64(0)`, and `sema_query_succeed`. Future
  calls hit `QUERY_BEGIN_CACHED` and serve the cached NULL.
  Diagnostic for parse-failure case still propagates correctly via
  the transitive dep on ast_query (which itself is in ERROR), so the
  cascade triggers when source becomes parseable again.
- **Discovered**: during #16 implementation, when B14's stat dump
  showed the lone error count and the user asked "what's that for?"
  Telemetry doing exactly what telemetry is supposed to do.

---

## Fixed during PR 1 / PR 2 (historical record — don't regress)

For each item: the root-cause and the test that catches the bug class.

### F1 — AST fingerprint was structural-only over (count, top-level kinds, top-level names)
- **Symptom**: Signature edits like `f :: fn() -> i32` → `f :: fn() -> u8`
  produced the same `ast_fp` because top-level name set was unchanged.
  Downstream signature queries served stale cached results.
- **Root cause**: Fingerprint chose "names exist" granularity; should have
  been content-addressed.
- **Fix**: PR 1 — `m->ast_fp = ii->source_fp` (source-byte hash already
  computed in `sema_set_input_source`).
- **Test**: T3 (signature edit) in `tools/sema_invalidation_test.c`.
- **Class**: Coarse fingerprint.

### F2 — Signature queries didn't record an AST dep
- **Symptom**: `query_fn_signature` / `query_struct_signature` /
  `query_enum_signature` read `di->origin` directly. Their slot's dep walker
  saw nothing changed when the AST was edited.
- **Root cause**: Untracked AST read.
- **Fix**: PR 1 — `sig_record_ast_dep(s, def)` at the top of each signature
  query body, plus `record_ast_dep_for_expr` in PR 2 const_eval.
- **Test**: T3 + the broader cascade tests.
- **Class**: AST-dep gap.

### F3 — `query_resolve_ref` didn't record an AST dep
- **Symptom**: Same NodeId from source A and source B (different ident name
  at the same position) hit the same cached entry. Cached DefId for `i32`
  was returned even when the new ident said `u8`.
- **Root cause**: Slot keyed by `(NodeId, Namespace)` but the *ident name*
  at that node can change between revisions; no AST dep meant no
  invalidation.
- **Fix**: PR 1 — `query_module_ast(s, mid)` call at the top.
- **Test**: T3 also covered this transitively.
- **Class**: AST-dep gap.

### F4 — `query_type_of_def` didn't record an AST dep
- **Symptom**: Value-bind path read `di->origin` via `type_of_value_bind`
  without recording a dep. Edits to a const-bind's RHS didn't invalidate
  the cached type.
- **Fix**: PR 1 — explicit `query_module_ast` after the GUARD.
- **Test**: covered by the chain of T13 in PR 2.
- **Class**: AST-dep gap.

### F5 — `sema_query_fail` didn't transfer frame deps to slot
- **Symptom**: A query that previously failed (e.g., looked up `new_name`
  before it was defined) stayed permanently failed, even after an edit
  that would make it succeed.
- **Root cause**: Failed slots had no recorded inputs; the revalidate walker
  couldn't tell when the cause-of-failure had changed.
- **Fix**: PR 1 — capture `top->deps` onto `slot->deps` and stamp
  `verified_rev` in `sema_query_fail`.
- **Test**: T6 (rename — new_name resolves after edit) and T12 (foo removed
  then re-added).
- **Class**: Slot-state asymmetry.

### F6 — `sema_query_begin`'s `QUERY_ERROR` case never re-validated
- **Symptom**: ERROR slots short-circuited to `QUERY_BEGIN_ERROR` without
  walking deps. Pair-bug to F5.
- **Fix**: PR 1 — mirror the DONE path: `sema_revalidate`; on RECOMPUTE,
  reset to EMPTY and `goto compute`.
- **Test**: T6, T12.
- **Class**: Slot-state asymmetry.

### F7 — `sema_revalidate` rejected non-DONE slots
- **Symptom**: ERROR slots passed to `sema_revalidate` returned
  `NOT_APPLICABLE` immediately, defeating F5+F6.
- **Fix**: PR 1 — accept both DONE and ERROR.
- **Test**: T6, T12.
- **Class**: Slot-state asymmetry.

### F8 — `sema_revalidate` swallowed transitive RECOMPUTE
- **Symptom**: The recursive walker called `sema_revalidate(dep_slot)` and
  discarded the result. Fingerprint compares against deps whose body
  hadn't actually re-run.
- **Fix**: PR 1 — capture `dep_result`, propagate `REVALIDATE_RECOMPUTE`.
- **Test**: T3, T9 cascade.
- **Class**: Slot-state asymmetry.

### F9 — `DefInfo.origin` was a stale `Expr*` after re-parse (4 sites)
- **Symptom**: `fn_lambda_for_def`, `struct_expr_for_def`, `enum_expr_for_def`,
  `is_fn_bind`/struct/enum, `type_of_value_bind`, `query_effect_ops_visible`,
  `query_fn_scope_index` all read `di->origin` directly. Got stale pointers
  after re-parse.
- **Fix**: PR 1 — migrated each to `def_origin(s, def)` which routes via
  `node_to_expr`.
- **Test**: T3 (signature) and the entire cascade.
- **Class**: Stale-cache pointer.

### F10 — `is_comptime_evaluable` was a non-query recursive walker
- **Symptom**: Quadratic on chains; can't track invalidation; editing
  `MAX :: 1024` to `MAX :: 2048` doesn't trigger D's comptime-recheck where
  D depends on a chain rooted at MAX.
- **Fix**: PR 2 — `query_is_comptime` with its own slot, fingerprint, and
  AST dep.
- **Test**: T14 (is_comptime flip) and T15 (no-op no-recompute on a 26-link
  chain).
- **Class**: Bypass of query system.

### F11 — `query_const_eval` didn't fold Idents, builtins, control flow
- **Symptom**: `let x: u8 = MAX` (where `MAX :: 1024`) silently passed
  range-check. `let x: u8 = @sizeOf(i32)` silently passed. Coerce saw
  CONST_NONE and took its structural-accept branch.
- **Fix**: PR 2 — added `expr_Ident`, `expr_Builtin` (primitives only),
  `expr_If`, `expr_Switch`, `expr_Block` cases.
- **Test**: T13 (chain invalidation), `comptime_chain_errors.ore` fixture.
- **Class**: Silent coerce hole.

### F12 — `bin_ops.c` only had `bin_add`
- **Symptom**: `MAX :: 1024 ; HALF :: MAX / 2` couldn't fold even after F11
  because `eval_bin` only dispatched `Plus`. Sub/Mul/Div/Mod silently
  returned CONST_NONE.
- **Fix**: PR 2 — added `bin_sub`, `bin_mul`, `bin_div`, `bin_mod` with
  overflow + div-by-zero guards.
- **Class**: Incomplete dispatch.

### F13 — `CONST_BOOL` didn't exist
- **Symptom**: `lit_True` / `lit_False` literals returned CONST_NONE.
  `unary_Not` had a comment saying "defer until bool literals flow through
  the const lattice." `if (true) ...` couldn't fold.
- **Fix**: PR 2 — added `CONST_BOOL` variant + `bool_val` union field.
  `eval_lit` handles `lit_True`/`lit_False`. `unary_Not` folds bools.
- **Class**: Incomplete lattice.

### F14 — Sema fields declared but never initialized / never read
- **Where**: `Sema.{anytype,effect_row,scope_token,effect}_type` and
  `Sema.name_{import,target,true,false,returnType}`.
- **Symptom**: Dead fields. `name_*` were intended for hot-path
  pre-interning but nothing initialized them; builtin dispatch did
  `pool_get + strcmp` every call.
- **Fix**: PR 1 hygiene — deleted the truly dead ones; initialized
  `name_{sizeOf,alignOf,TypeOf,intCast,typeName}` in `sema_init` and
  rewired `type_of_builtin` to compare uint32_t IDs.

### F15 — `Fn`/`fn` split: capital `Fn` is type-only, lowercase `fn` is value-only
- **Symptom**: `expr_Lambda` was dual-purpose. The same AST node
  meant either "function value" (lambda body) or "function type"
  depending on which sema function called it. The parser packed
  bare `fn(i32) -> i32` as `Param{name=i32, type_ann=NULL}` because
  Param requires a leading identifier. To recover the type from
  that, sema's `resolve_type_expr` Lambda case had a primitive-name
  heuristic that worked for `fn(i32)` but rejected `fn(Point)` —
  see B17 for the user-visible failure.
- **Fix**: Lexer adds `Fn` keyword → new `FnType` token. AST adds
  `expr_FnType` with a clean `FnTypeExpr { Vec* param_types,
  Expr* ret_type }` (no Param wrapper, no names). Parser dispatches
  `Fn` to `parse_fn_type` which reads comma-separated type
  expressions directly. Sema's `resolve_type_expr` adds an
  `expr_FnType` case (~25 lines, straight-line) and the old
  `expr_Lambda` case (45 lines including the heuristic) is
  **deleted**. `expr_Lambda` is now a value-only AST node.
- **Class**: Language-design cleanup; eliminates an entire
  bug-class of "is this a name or a type" parser ambiguity.
- **What got deleted**:
  - The primitive-name heuristic in `resolve_type_expr`
    ([src/sema/type/checker.c:120-145](src/sema/type/checker.c) — pre-fix line numbers).
  - The dual-purpose dispatch on `expr_Lambda` in
    `resolve_type_expr` (the case is now an
    "expected `Fn`, got `fn`" diagnostic).
  - Future-proof: nothing else needs to special-case Lambda for
    type vs. value distinction.
- **What got added**:
  - `Fn` token in the lexer ([src/lexer/lexer.c](src/lexer/lexer.c)) and
    `FnType` enum value ([src/lexer/token.h](src/lexer/token.h)).
  - `expr_FnType` + `FnTypeExpr` in
    ([src/parser/ast.h](src/parser/ast.h)).
  - `parse_fn_type` parser case in
    ([src/parser/parser.c](src/parser/parser.c)).
  - `expr_FnType` case in `resolve_type_expr`
    ([src/sema/type/checker.c](src/sema/type/checker.c)).
  - `expr_FnType` case in two AST-walking passes
    ([src/sema/resolve/scope_index.c](src/sema/resolve/scope_index.c)).
- **Test**: `examples/tests/fn_in_type_position.ore` now exercises
  anonymous user-defined types (`Fn(Point) -> i32` and
  `Fn(Point) -> Point`) — pre-fix would error. The failure mode
  for the old syntax (`fn(...)` in type position) is a clear
  diagnostic pointing at the right keyword.
- **Known partials filed in same PR**: `Fn(...) <Exn> -> R` (effect-
  annotated fn type) is not yet supported — current `Fn` parser
  doesn't accept the `<...>` block. Effect-annotated fn types in
  `tools/test.sh`'s ad-hoc fixtures stay on lowercase `fn` for now;
  test.sh is currently broken (B1) so they're not exercised.
- **Class**: Dead infrastructure.

---

## Deferred design decisions

These are design choices we considered, evaluated against current
needs, and consciously chose not to act on. Distinct from "open
bugs" and "known partials" — they're notes for future contributors
so a real signal (an actually-expensive intrinsic, an actual
performance problem) is what re-opens the question, not a fresh
re-derivation of the trade-off.

### D1 — `query_intrinsic` deferred until there's an expensive intrinsic
- **Question raised**: Should builtin / intrinsic calls (`@sizeOf`,
  `@alignOf`, `@TypeOf`, `@intCast`, `@typeName`, `@returnType`)
  have their own per-`(intrinsic_kind, args)` slot so that
  structurally-identical-but-AST-distinct call sites (e.g., two
  separate `@sizeOf(i32)` exprs) share a single cached result?
- **Answer**: No, not now.
- **Why**:
  1. **Today's intrinsics are all O(1) or already-cached.** `@sizeOf`
     / `@alignOf` for primitives is a static table lookup; for
     aggregates it'll chain through `query_layout_of_type` (PR 2.5)
     which is itself slot-cached and keyed by interned `Type*` —
     dedup happens there. `@TypeOf` chains through
     `query_type_of_expr` which is cached. `@intCast` is a coerce.
     `@typeName` is a string format.
  2. **Per-Expr caching is already in place.** Each `expr_Builtin`
     AST node hits `query_type_of_expr`'s slot (cached by NodeId)
     and `query_const_eval`'s slot (also by NodeId). Adding a
     dedicated intrinsic slot doesn't dedupe the *work* — it'd
     dedupe the *containing-Expr* level.
  3. **Real dedup across distinct call sites would need content
     hashing**, not slot-keying — that's what type interning already
     buys for the args. We don't need a new layer.
  4. **Permanent maintenance cost is non-zero.** Every new slot
     kind needs care in `sema_locate_slot`, `sema_query_kind_str`,
     the invalidation walker, future verified_at/changed_at split
     work, etc.
- **When to revisit**: a future intrinsic with non-trivial
  computation. The motivating case would be something like
  `@reify(T)` (struct introspection — fields, methods, layout)
  where two calls on the same `T` should clearly share work.
- **Implementation sketch (when needed)**: ~80 LoC mirroring
  `ConstEvalEntry`'s shape. Key on `(BuiltinKind, Type*[])` so the
  args contribute pre-interned pointers — no need to walk arg ASTs
  at lookup time.

---

## Reference cross-checks (what zig / rust-analyzer do better)

### R1 — Salsa's `verified_at` / `changed_at` split
- **Where (theirs)**: `rust-analyzer/crates/salsa/src/derived/slot.rs`.
- **Where (ours)**: [src/sema/query/query.h:106-124](src/sema/query/query.h#L106-L124)
  has `computed_rev` and `verified_rev` already as proto-fields, but they're
  used as a single timestamp pair, not the full split.
- **What it buys**: A query can re-run and produce the same result
  (changed_rev unchanged), letting downstream skip recompute via early-cutoff
  at the slot level — finer than fingerprint comparison alone.
- **Ore status**: `cleanup.md #15` — deferred to post-E.4.

### R2 — Salsa's `report_untracked_read`
- **Where (theirs)**: `rust-analyzer/crates/salsa/src/runtime.rs`.
- **What it buys**: Every read inside a query body asserts a dep was
  recorded. Catches the entire AST-dep-gap class (F2, F3, F4) statically.
- **Ore status**: `cleanup.md #16` — deferred to its own PR alongside #22.

### R3 — Salsa's `CycleRecoveryStrategy::Fixpoint`
- **Where (theirs)**: `rust-analyzer/crates/salsa/src/cycle.rs`.
- **What it buys**: Self-referential types compute via fixpoint instead of
  cycle-as-error. Required for generics in E.5.
- **Ore status**: B13 above.

### R4 — Zig's intern pool for ConstValue
- **Where (theirs)**: `zig/src/Sema.zig` — `resolveValue`, `Air.Inst.Ref`.
- **What it buys**: Every comptime-known value lives in an intern pool;
  composition is mechanical ("if both operands are interned, result is
  interned"). Solves B7 (uninitialized union bytes), enables ConstValue
  payloads for struct/array/string without churn.
- **Ore status**: PR 2.5 — when struct/array/string payloads are needed,
  pull this design forward.

### R5 — Zig's `abiSize` for layout `[FIXED in PR 3.5]`
- **Where (theirs)**: `zig/src/Sema.zig` — `zirSizeOf` calls
  `ty.abiSize(zcu)`.
- **What it bought**: Closed the aggregate `@sizeOf` / `@alignOf`
  hole. Pre-R5, `@sizeOf(Point)` and `@sizeOf([10]u8)` both returned
  `CONST_NONE`; coerce silently accepted any target type.
- **Implementation**: New `src/sema/type/layout.{h,c}` with
  `query_layout_of_type(t) → Layout { size, align, is_known }`.
  Slot-cached per `Type*` (interned, so pointer identity = type
  identity); cycles via the standard SEMA_QUERY_GUARD RUNNING-state
  detection. Refactored `const_eval`'s `expr_Builtin` case to call
  the layout query instead of the static primitive table.
- **Coverage**: primitives, pointers (`^T`, `[^]T` — 8 bytes LP64),
  slices (16 bytes for `{ ptr, len }`), arrays (`N × elem_size`,
  alignment of elem), optionals (niche on pointer-likes,
  tagged on others), structs (C-style alignment with padding),
  enums (smallest int that fits the variant range), function
  pointers.
- **Cycle handling**: direct (`Bad :: struct { self: Bad }`) and
  transitive (`Mid → End → Mid`) by-value cycles produce precise
  per-field diagnostics suggesting `^T` / `?^T` indirection.
- **Tests**:
  - `examples/tests/comptime_aggregate_size.ore` — 25+ folded
    aggregate sizes/aligns including struct padding (8/12/3 byte
    structs), arrays of primitives and structs, optional niches
    and tags, enum width-by-range, function-pointer size,
    pointer-recursive `Node :: struct { value: i32; next: ?^Node }`.
  - `examples/tests/comptime_aggregate_size_errors.ore` — direct
    self-cycle, transitive cycle through two intermediate structs,
    and an aggregate `@sizeOf` that overflows u8 (the case that
    pre-R5 coerce silently accepted).
- **Status**: **FIXED**.

### R6 — Per-decl AST node fingerprint
- **Where (theirs)**: rust-analyzer's `HirFileId` + AST node has its own
  fingerprint independent of the whole-file source hash.
- **What it buys**: Body-only edits invalidate exactly one signature
  query, not all of them. Today our PR 1 fix uses `source_fp` for
  `ast_fp`, which over-invalidates.
- **Ore status**: PR 4+ optimization. Correctness-equivalent today;
  perf-relevant when modules grow.

---

## Pattern catalog (what to look for in code review)

A new query, cache, or AST reader should be checked against:

1. **Does it record an AST dep?** (F2-F4 class.) Either via
   `record_ast_dep_for_expr` / `sig_record_ast_dep` (B8 — should be one
   helper) or by calling another query that does.
2. **What's its slot fingerprint?** Coarse fingerprints (B9, F1) silently
   mask edits. Fingerprint over the *structurally meaningful* output.
3. **What does it return on cycle?** A sentinel is fine for pre-E.5 (B13);
   make sure it's documented and that the caller handles the sentinel.
4. **What does it return on error?** Pair check with sentinel-on-cycle.
   Failed slots need to capture deps for ERROR-revalidation (F5-F7).
5. **Reads `di->origin`?** Use `def_origin(s, def)` (F9). Plain reads
   silently break across re-parses.
6. **Allocates side-table state on success?** Check the unwind path on
   slot failure / removal (B4, B5, B12). The accumulator pattern from
   `refs_to_def` is the right model.
7. **Reads `s->some_field` directly?** Suspect untracked-read (R2,
   `cleanup.md #16`).
8. **Tagged union hashed by blob?** B7. Switch on tag, hash active
   variant only.
9. **Driver flow runs it?** B2. Type checking has to be wired into the
   driver, not a debug dump.

---

## PR routing — which entries land where

How the open / latent items map onto the next several PRs. Update this
table when an entry moves homes.

```
PR 3 (Layer 3 type system: coerce variance, ?T, fn-in-type, [^]T arith,
       bidirectional through if/switch, string → []const u8) ✓ DONE
  ├── Layer 0 (gating prereqs):
  │     B2 ✓ — driver typecheck (FIXED)
  │     B7 ✓ — const_eval fingerprint UB (FIXED)
  └── Layer 1: cleanup.md #5-#10 ✓ all six features landed
        Surfaced and filed B15-B19 as deliberate scope-cuts (below).

PR 3.5 ✓ DONE (close the type-system known-partials)
  ├── B15 ✓ — `nil → ?T` literal coercion + TY_NIL primitive (FIXED)
  ├── B16 ✓ — `?T` unwrapping ops (`?` postfix, `orelse`) (FIXED)
  ├── R5 ✓ — abiSize for aggregate `@sizeOf` / `@alignOf` (FIXED)
  └── R4 — Zig intern pool for ConstValue — DEFERRED again. Not
           needed for B15/B16/R5 in their PR 3.5 form. Reopen if/when
           ConstValue gains struct/array/string payloads.

Edit-cycle hygiene PR ✓ DONE (collapsed scope after test-first
  investigation; see B3/B4/B5/B12 fix notes)
  ├── B12 ✓ — proven correct by T16; was already fixed by PR 1's
  │           DefMapEntry reuse path
  ├── B3, B4, B5 ✓ — correctness verified; remaining concern is
  │           memory bloat, deferred to generational-arena work
  │           (rust-analyzer-style per-revision arenas) when LSP
  │           scale demands it.
  ├── T16 ✓ — rename → re-add → original name fold value verified
  ├── T17 ✓ — 20-edit stress with adds + removes
  └── T18 ✓ — same-name-different-shape across revisions
  LSP shell work no longer gated by this PR.

#16 follow-up PR ✓ DONE (untracked-read trace mode + query observability)
  ├── B8 ✓ — ast_dep helper consolidation into src/sema/query/ast_dep.{h,c}
  ├── B9 ✓ — query_def_for_name slot fingerprint (four-tuple over
  │           DefId.idx + sem + vis + bind_kind)
  ├── B11 ✓ — def_origin precondition assert under ORE_DEBUG_QUERIES
  ├── B14 ✓ — per-QueryKind telemetry + --dump-query-stats
  ├── R2 ✓ — SEMA_READ_UNTRACKED macro + has_untracked_read flag +
  │          REVALIDATE_RECOMPUTE branch in invalidate
  └── Migration audit: 0 sites needed wrapping today. Codebase already
       routes through query_* helpers (PR 1's discipline). Macro stands
       as future-use infra. Surfaced B20 + B21 as separate PRs (below).

PR 4 ✓ DONE (resolution / scope cleanup, original cleanup.md #11-#14)
  ├── B6 ✓ — NS_VALUE_OR_TYPE consolidates 2-slot fallback callers
  │           into 1 slot. expr_check, const_eval(is_comptime),
  │           const_eval(expr_Ident) all share the slot now.
  ├── B18 ✓ — `[^]T - [^]T → usize` + ptr equality (landed alongside
  │           B20/B21 cleanup; followed Zig's policies).
  └── B19 ✓ — coerce `emit` flag refactored away. Now
              `coerce_structural` (pure CoerceResult predicate) +
              `coerce` (thin emitting wrapper). Optional-lift
              recursion no longer needs flag-threading.

#22 follow-up PR (diagnostic codes + phrasing)
  └── No bug_of_bugs entries map directly. Optional: + B1 if lumping tooling.

E.5 (generics)
  └── B13 — cycle fixpoint recovery (already cleanup.md #17)

PR 4+ (DefInfo refactor — long-term)
  └── B10 — DefInfo as truly immutable identity; move re-parse-affected
            fields into a side query (query_def_origin, etc.). Big
            refactor; pair with B21 if both land in one cycle.

Reference targets (long-term)
  ├── R1 — verified_at/changed_at split (cleanup.md #15, post-E.4)
  ├── R3 — cycle fixpoint — see B13 / E.5 above
  ├── R4 — Zig intern pool for ConstValue (reopen if/when ConstValue
  │        gains struct/array/string payloads)
  └── R6 — per-decl AST fingerprint (Layer 4+ optimization, only
            matters at LSP scale)

B20 + B21 + B18 cleanup PR ✓ DONE (architectural alignment with Salsa
  + pointer-op completeness; B20/B21 surfaced during #16 audit, B18
  ridden in for natural fit)
  ├── B20 ✓ — query_top_level_index now succeeds-with-NULL on absent
  │           AST instead of failing. Telemetry's `error` column is
  │           now zero across passing fixtures. Scope corrected:
  │           query_module_ast bypass is structurally necessary
  │           (no InputInfo for primitives = no slot to address).
  ├── B21 ✓ — query_node_to_decl_index promoted to real slot-cached
  │           per-module query. Lookups query_node_to_decl /
  │           query_scope_for_node take Expr* (so they can resolve
  │           owning module via span and record the producer dep).
  │           Telemetry's new `node_to_decl` row: 30 begin / 29 cached
  │           / 1 compute on a 26-link chain.
  └── B18 ✓ — `[^]T - [^]T → usize` pointer subtraction +
              `[^]T == [^]T / !=` equality. Followed Zig's
              analyzeArithmetic + isSelfComparable policies: usize
              result, elem-type intern-equality required, ordering
              ops rejected for raw pointers. Iterator-style `cur != end`
              code now typechecks.

Build infrastructure ✓ DONE (Nix flake + IDX dev.nix landed; B1 fixed
  alongside as `make test` was rewritten as a slim smoke runner)
  ├── flake.nix ✓ — multi-platform devShell, zig 0.16.0 pinned via
  │                 mitchellh/zig-overlay
  ├── .idx/dev.nix ✓ — Firebase Studio (IDX) mirror of the package list
  ├── B1 ✓ — tools/test.sh rewritten (1400 LoC → ~80 LoC); UBSan
  │           replaces ASan as portable workaround for B22
  └── B22 — `zig cc -fsanitize=address` link failure on macOS;
            external Zig issue, periodically re-test on version bumps
```

The "Edit-cycle hygiene PR" is the only new PR slot this analysis adds —
the four items in it (B3, B4, B5, B12) are interlocked and were surfaced
by PR 1's harness scenarios (T6 rename, T11 multi-edit stress, T12
re-add removed). They don't fit the type-system focus of PR 3 or the
infrastructure focus of #16, but they DO fit cleanly as one cohesive
fix because B5 falls out of B4 and B12 falls out of both.

---

## Postscript

This list is alive — every PR should add to it. The bug-class taxonomy is
deliberately rough; sharpen it as patterns recur. The numbers (B1, F1, R1)
are stable references — don't renumber when you delete or merge entries;
mark them `[DELETED — see Bn]` instead.

Cross-reference convention: when `cleanup.md` items relate to one or more
bug_of_bugs entries, annotate the cleanup.md entry inline:
`(see bug_of_bugs F1, F2 — proven by harness T1-T15)`. Keeps the two
living docs tied without duplication.

Last updated: end of #16 follow-up PR. B8/B9/B11/B14/R2 all FIXED.
B17 reconfirmed FIXED (Fn/fn split). B20/B21 filed as small
follow-ups discovered during the #16 audit. PR routing table
updated: #16 marked DONE, #16's "~585 SEMA_READ migrations" line
removed (audit found 0 sites today; macro stands as future-use
infra), B17 removed from PR 4 lineup.

Two recurring lessons across PR 1 → #16:

1. **Write the test FIRST.** Items B3/B4/B5/B12 were filed
   speculatively before PR 1's reuse-path fix had been fully
   reasoned about; three tests later (T16/T17/T18) the entire
   "edit-cycle hygiene PR" was just a docstring update. Same
   pattern in #16's migration: the ~135-site estimate was
   speculation; the audit found 0. When a "this might be a
   problem" concern is filed, write the test (or do the audit)
   that would prove it before allocating effort to the fix.

2. **Telemetry pays for itself immediately.** B14's stat dump
   surfaced B20 the first time it ran — a passing fixture had
   `1` in the `error` column for `top_level_index` (primitives'
   sourceless top-level index correctly bailing out). The
   architectural mismatch was invisible until the dump made it
   visible. Worth instrumenting *before* you think you have a
   problem to find.
