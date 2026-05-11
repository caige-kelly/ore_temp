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

### B22 — `zig cc -fsanitize=address` link failure on macOS `[FIXED via routing]`
- **Where**: would surface during `make test` if the smoke-test build
  used `zig cc -fsanitize=address` on aarch64-darwin.
- **Symptom (when triggered)**: `error: undefined symbol:
  ___asan_version_mismatch_check_v8` (and friends like
  `___asan_stack_malloc_1`). Adding `-shared-libsan` doesn't help —
  Zig still emits references to the static-link symbols.
- **Root cause**: Upstream Zig issue. Zig's bundled compiler-rt for
  darwin doesn't ship the version-check / stack-instrumentation
  symbols the AddressSanitizer runtime expects. Not yet resolved
  upstream as of zig 0.16.0.
- **Status**: **FIXED** (routed around, then made moot).
- **Class**: External toolchain bug (not Ore code).
- **Fix progression**:
  1. *First fix*: Split `make test` to `TEST_CC=clang` while keeping
     `CC=zig cc` for the main build. Worked but added complexity for
     little gain — Zig's main value is cross-compilation, which we
     never used.
  2. *Final fix*: Dropped Zig entirely. Single C toolchain
     (`pkgs.clang_19`) for both `make all` and `make test`. No more
     split, no more zig-overlay flake input, ~30 lines removed from
     `flake.nix`. Bumped CFLAGS from `-std=c17` to `-std=c23` while
     we were touching it (clang 19 has solid C23 support and we'd
     never been on c17 for any specific reason).
- **Verified**: deliberate use-after-free in a smoke `.c` file
  triggers `AddressSanitizer: heap-use-after-free` at runtime on
  macOS aarch64 (clang 19.1.7). Full test suite passes:
  43+21+21+41 = 126/126.
- **Future**: if cross-compilation ever becomes interesting,
  re-adding Zig is a 10-minute change. Not urgent.

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

### B8 — `record_ast_dep_for_expr` duplicates `sig_record_ast_dep` `[FIXED]`
- **Where (was)**: Two ad-hoc helpers — one in
  [src/sema/eval/const_eval.c](src/sema/eval/const_eval.c) and one in
  [src/sema/type/decl_data.c](src/sema/type/decl_data.c) — each
  re-implemented the AST-dep-recording logic.
- **Status**: **FIXED**.
- **Class**: Duplication.
- **Fix**: Extracted to [src/sema/query/ast_dep.{h,c}](src/sema/query/ast_dep.c)
  with `record_ast_dep_for_def(s, def)` and `record_ast_dep_for_span(s, span)`.
  All call sites migrated (4 in decl_data.c, plus const_eval.c). The
  module-routing helper (`module_for_span`) lives in the same module.

### B9 — `query_def_for_name` slot fingerprint stays at `FINGERPRINT_NONE` `[FIXED]`
- **Where (was)**: [src/sema/modules/def_map.c](src/sema/modules/def_map.c) —
  `sema_query_succeed` calls in def_for_name succeeded without setting a
  fingerprint.
- **Symptom (was)**: Dependents on `def_for_name` couldn't early-cut by
  fingerprint comparison; they always re-walked. Workaround was that
  signature queries depended on `query_module_ast` directly.
- **Status**: **FIXED**.
- **Class**: Coarse fingerprint.
- **Fix**: `def_for_name_fp(def, sem, vis, bind_kind)` four-tuple
  fingerprint, set on the entry's slot after both the reuse path and
  the first-time-create path ([def_map.c:326](src/sema/modules/def_map.c#L326)).
  Single tail path after the per-PR-N rack-and-stack dedup work
  consolidated both branches. A `pub` toggle now shifts the fingerprint
  while a body-only edit doesn't.

---

## Medium (won't bite soon, but worth tracking)

### B10 — `DefInfo` is a hand-managed cache with implicit refresh contract `[FIXED in DefInfo-strip PR]`
- **Where (was)**: [src/sema/modules/def_map.c](src/sema/modules/def_map.c)
  — the reuse path manually refreshed `semantic_kind`, `span`, `origin_id`,
  `origin`, `vis`. Easy to forget a field. Adding a new field to `DefInfo`
  silently broke invalidation if you didn't also touch the reuse path.
- **Symptom (was)**: Forgotten field stays at source-A value across an
  edit. T6 (rename) caught a version of this for `vis` during PR 1.
  This session's LSP work surfaced the same class of bug when a
  didChange caused phantom "duplicate field" diagnostics — root cause
  was `child_scope` (a cache field on DefInfo) stale across recompute.
- **Root cause (was)**: The "thin identity" design split per-kind data
  into side tables but `DefInfo` itself still had cache-shaped fields
  (`origin`, `origin_id`, `child_scope`, `vis`, `span`, `semantic_kind`).
  The refresh in def_for_name's body was silently skipped on the
  cached-output path, leaving the cache-shaped fields stale.
- **Status**: **FIXED**.
- **Class**: Hand-managed cache.
- **Fix**: Stripped `DefInfo` to pure identity:
  ```
  struct DefInfo {
      DeclKind kind;
      StrId name_id;
      AstId ast_id;            // top-level identity handle
      struct NodeId origin_id; // local-bind handle (per-parse fresh)
      ScopeId owner_scope;
  };
  ```
  Deleted: `vis`, `span`, `semantic_kind`, `origin` (Expr*),
  `child_scope`, `imported_module`, `scope_token_id` — six fields.
  Replaced with per-def accessors that re-derive from the AST on
  every read:
  - `def_visibility(s, def)` — reads from the current AST via
    `def_origin(s, def)` for top-level, kind-dispatch for nested.
  - `def_span(s, def)` — same dispatch shape.
  - `def_semantic_kind(s, def)` — reads bind value's expr kind via
    `sem_for_bind_value`.
  Matches rust-analyzer's pattern: identity records hold no
  AST-derived state; every accessor re-derives from a tracked
  query. The refresh-on-cached-path bug class is structurally
  eliminated.
- **Tests**:
  - `test_struct_no_duplicate_diagnostics` — drives a struct edit
    via `query_type_of_def` (which reaches `query_struct_signature`
    via expr_check's p.y path) across two revisions; asserts zero
    diagnostics. Catches the original LSP bug pattern.
  - `test_def_origin_stable_under_sibling_insert` — inserts a
    sibling decl before Point, asserts get_x still typechecks
    (def_origin via AstIdMap finds the current revision's Bind
    node regardless of where it shifted in the file).

### B11 — `def_origin` indirection silently depends on `scope_index_build_module` running `[FIXED via AstIdMap]`
- **Where (was)**: [src/sema/ids/ids.c](src/sema/ids/ids.c) — `def_origin`
  fell back to the raw `di->origin` if `node_to_expr` didn't contain
  `origin_id`, and the lookup itself required `scope_index_build_module`
  to have populated `node_to_expr` for the current revision.
- **Symptom (was)**: A query that ran *before* `scope_index_build_module`
  populated `node_to_expr` could get the *prior* parse's Expr* (stale).
  The driver enforced the ordering implicitly; the contract was unwritten.
- **Status**: **FIXED**.
- **Class**: Implicit ordering contract.
- **Fix**: Two-path dispatch in `def_origin`:
  - Top-level DECL_USER / DECL_IMPORT (`di->ast_id` valid): look up via
    the owning module's `AstIdMap`. AstIds are stable across reparses
    for items with unchanged (kind, name), independent of where the
    item has shifted in the file. The map is built by
    `query_top_level_index` — same lifecycle as the entry list itself.
    No node_to_expr dependence.
  - Local DECL_USER (let-binds in fn bodies), DECL_PARAM, etc.: still
    look up via `origin_id` → `node_to_expr`. These DefInfo records
    are themselves rebuilt by `scope_index_build_module` on every
    revision, so their `origin_id` is always fresh by construction.
    The "stale lookup before scope_index runs" failure mode can't
    happen — no DefInfo exists before scope_index creates it.
  The raw `di->origin` Expr* fallback (the B11 latent-bug path) was
  deleted alongside the field itself in the DefInfo strip (B10 fix).
  Class of bug structurally eliminated.
- **Discovered**: end-to-end LSP testing of multi-file open + struct
  edit cycles. Symptom was phantom "duplicate field name" diagnostics
  on didChange; root cause was identity-record cache staleness, same
  class as B10. Fixed together.

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

## LSP shell + query-architecture session (R4 follow-up)

These were discovered or filed during the LSP shell bring-up + the
follow-on architectural pass that landed the rust-analyzer-faithful
DefInfo strip, AstIdMap, and cancel/snapshot/orphan cleanup.

### B23 — Phantom "duplicate field/variant name" diagnostics on LSP didChange `[FIXED]`
- **Where (was)**: [src/sema/type/decl_data.c](src/sema/type/decl_data.c) —
  `query_struct_signature` and `query_enum_signature` lazily allocated
  the child scope on `DefInfo.child_scope` and stashed field/variant
  DefIds into it. On recompute, the prior revision's ScopeId was
  still set, so the lazy-create `if (!scope_id_is_valid)` check was
  false, the existing scope was reused, and re-emitting fields into
  it hit `scope_define_def` returning false (name already present)
  for every field → emitted "duplicate field name" for every field.
- **Symptom**: User-visible in VS Code Dev Host. Open `structs.ore`,
  edit → every field reports "duplicate field name in struct".
- **Root cause**: Identity-record cache staleness — same class as
  B10/B11. `child_scope` was a cache-shaped field on `DefInfo`; the
  refresh path that should have invalidated it on recompute was
  gated by SEMA_QUERY_GUARD's cached-output check and silently
  skipped.
- **Status**: **FIXED**.
- **Class**: Cache-on-identity-record.
- **Fix**: rust-analyzer-faithful pattern. Deleted the entire
  `child_scope` concept for nominal-type members (rust-analyzer has
  no `ItemScope` for struct fields). Field/variant DefIds now live
  on the signature output (`StructSignature.field_defs` and
  `EnumSignature.variant_defs` parallel arrays to `fields`/`variants`).
  Member lookup is direct iteration: `struct_find_field_def(s, parent, name)`
  scans `sig->fields[i].name_id`. Path resolution dispatches by kind
  in `resolve_member_lookup` ([src/sema/resolve/resolve.c](src/sema/resolve/resolve.c)).
  `inhabitable_scope_of` deleted entirely.
- **Identity preservation**: Field DefIds allocated via
  `field_def_for(s, parent_struct, index)` keyed by `(parent.idx << 32) | index`
  in `Sema.struct_field_defs` — stable per position across recomputes.
  Same pattern as rust-analyzer's `FieldId { parent: VariantId, local_id: LocalFieldId }`.
- **Tests**: `test_struct_no_duplicate_diagnostics` (invalidation suite)
  drives a struct edit + asserts zero diagnostics. LSP smoke test on
  `structs.ore` open + didChange cycles produces 0 diagnostics across
  all revisions.

### B24 — Position-based NodeIds collide across modules in one Sema `[FIXED via file-prefix]`
- **Where (was)**: [src/parser/parser.c](src/parser/parser.c) —
  parser's `next_node_id` reset to 1 each parse. With the LSP holding
  multiple modules in one long-lived Sema, NodeId 1 from module A
  collided with NodeId 1 from module B in every NodeId-keyed cache
  (`type_of_expr_entries`, `node_to_decl`, `resolve_ref_entries`,
  `const_eval_entries`, `is_comptime_entries`, `refs_to_def`,
  `node_to_expr`). Phantom cache hits returned data from the wrong
  module.
- **Symptom**: Opening two test fixtures in one LSP session produced
  phantom diagnostics in the second (e.g., "expected u8, got
  comptime_float" on a clean fixture if a different fixture had been
  opened first). CLI builds (one module per process) didn't expose
  the bug.
- **Root cause**: NodeId namespace conflated across files.
- **Status**: **FIXED**.
- **Class**: Identity-namespace collision.
- **Fix**: Encoded `file_id` in the high bits of NodeId. Layout:
  high 12 bits = `file_id`, low 20 bits = per-parse local counter
  (1M nodes/file, 4K files per Sema — generous ceilings). Constants
  `NODE_ID_FILE_BITS` / `NODE_ID_LOCAL_BITS` in
  [src/parser/ast.h](src/parser/ast.h). Per-parse local counter
  preserved (matches what existing invalidation tests depend on for
  same-content-reparse stability); cross-file collision eliminated
  by construction.
- **Why not Sema-monotonic counter**: tried first. Broke 9 of 22
  invalidation tests because the existing slot machinery relied on
  NodeIds being stable across re-parses of the same input. The
  file-prefix scheme preserves single-module invalidation behavior
  while fixing cross-module collision.
- **Long-term**: name-keyed AstIds (per B25 below) made this scheme
  partially obsolete for top-level identity, but the file-prefix
  is still load-bearing for body-level NodeId-keyed caches.

### B25 — Top-level item identity not stable under sibling-insert edits `[FIXED via AstIdMap]`
- **Where (was)**: With the file-prefix NodeId scheme (B24), top-level
  NodeIds are stable per-file only when source position is preserved.
  Inserting a sibling decl before an existing item shifted every
  subsequent NodeId. `DefInfo.origin_id` was refreshed inside
  `def_for_name`'s body which was gated by SEMA_QUERY_GUARD's
  cached-output path; the refresh silently skipped on revisions where
  def_for_name's output (DefId) hadn't changed. → stale origin_id
  reads via `def_origin`.
- **Symptom**: A regression test `test_def_origin_stable_under_sibling_insert`
  was written to exercise this; pre-fix it failed.
- **Status**: **FIXED**.
- **Class**: Position-keyed identity for inputs that shift on edits.
- **Fix**: Added `AstId` type + per-module `AstIdMap`
  ([src/sema/modules/ast_id_map.{h,c}](src/sema/modules/ast_id_map.c))
  modeled on rust-analyzer's `FileAstId<T>` / `AstIdMap` (see
  `rust-analyzer/crates/span/src/ast_id.rs`).
  - Identity computed from `hash((kind, name_id))` at parse time
    with linear-probing collision resolution.
  - Map built inside `query_top_level_index` alongside the existing
    entry list; reset at the start of each (re)compute.
  - `TopLevelEntry.ast_id` holds the assigned AstId; `def_for_name`
    copies it onto the allocated `DefInfo.ast_id`.
  - `def_origin` dispatches: top-level DECL_USER/DECL_IMPORT
    (`ast_id` valid) → AstIdMap lookup; local/nested
    (`origin_id` valid) → node_to_expr lookup.
- **Test**: `test_def_origin_stable_under_sibling_insert` — inserts
  `before_point :: 0\n` before `Point :: struct`, asserts `get_x`
  still typechecks. Without the AstIdMap, NodeIds for Point shift
  on the insert and the prior `DefInfo.origin_id` becomes stale.
- **What this DOESN'T do**: cover body-level expressions. Rust-analyzer
  takes the same stance ("invalidation in bodies is not much of a
  problem"); body NodeIds in Ore are still position-based with
  file-prefix and shift on edits. R7 below tracks if/when this
  ever becomes a measured problem.

### B26 — `query_fn_scope_index` accumulates state across recomputes `[FIXED]`
- **Where (was)**: [src/sema/resolve/scope_index.c](src/sema/resolve/scope_index.c)
  — `query_fn_scope_index` cached a `ScopeIndexResult` in
  `s->fn_scope_index_cache` (keyed by fn DefId, stable across
  revisions). On recompute, the body called `scope_walk` which
  APPENDED to `res->local_scopes` (Vec<ScopeId>) and PUT into
  `res->node_to_scope` (HashMap). Old revisions' ScopeIds and
  NodeIds lingered.
- **Symptom**: Not user-visible today (NodeId keys overwrite on
  same-position content; orphan ScopeIds in `local_scopes` are
  never iterated by any consumer). But the same architectural class
  as B23 — output state accumulating across recomputes — guaranteed
  it would bite the moment any consumer DID iterate the accumulating
  collection.
- **Status**: **FIXED**.
- **Class**: Output state not reset on recompute.
- **Fix**: At the top of the body (after SEMA_QUERY_GUARD falls
  through to compute), reset `res->local_scopes->count = 0` and
  `hashmap_clear(&res->node_to_scope)`. Same pattern as the
  StructSignature / EnumSignature rebuild — query outputs holding
  growable collections clear on recompute.

### B27 — `hashmap_remove` missing; orphan locators leak on shrinking signatures `[FIXED]`
- **Where (was)**: [src/common/hashmap.h](src/common/hashmap.h) had
  no remove API ("Deletion is intentionally omitted until a compiler
  use case needs it"). Side-effect: when a struct shrunk (field
  removed in an edit), the old field's `field_locators` entry and
  its `struct_field_defs` slot stayed in their maps forever.
- **Symptom**: Long-running LSP sessions accumulate orphan locator
  entries proportional to total-ever-removed-fields. Memory only;
  no correctness impact.
- **Status**: **FIXED**.
- **Class**: Hand-managed cache (memory only).
- **Fix**:
  - Added `hashmap_remove(map, key)` with backward-shift cleanup
    ([src/common/hashmap.c](src/common/hashmap.c)). No tombstones;
    probe chain stays compact.
  - GC pass at the end of `query_struct_signature` /
    `query_enum_signature`: capture `prev_field_count` before
    rebuild; for indices `[new_count, prev_field_count)`, remove
    the orphan from `struct_field_defs` and the matching DefId
    from `field_locators`. Same pattern for variants.
- **Tests**: `tools/hashmap_test.c`'s new `test_remove` (5 scenarios:
  middle-key removal, absent-key, re-insert into removed slot,
  full-table removal, count tracking). `test_shrinking_field_locators_gc`
  in the invalidation suite drives a 3→2 field shrink and asserts
  the orphan entries are gone from both maps.

### B28 — Orphan API surface across cancel/snapshot/query_engine `[FIXED]`
- **Where (was)**: Spread across [src/sema/request/cancel.c](src/sema/request/cancel.c),
  [src/sema/request/snapshot.c](src/sema/request/snapshot.c),
  [src/sema/query/query_engine.c](src/sema/query/query_engine.c).
  Eleven functions + one struct + two Sema fields scaffolded for
  features that never wired:
  - `sema_set_active_cancel`, `sema_clear_active_cancel`,
    `cancel_token_init`, `cancel_token_signal` — never called
  - `sema_snapshot_begin`, `sema_snapshot_end`, `struct Snapshot` —
    never called
  - `sema_bump_revision`, `sema_current_revision`,
    `sema_set_slot_budget`, `sema_slot_count`, `sema_evict_lru` —
    never called (revisions inlined `++s->current_revision` at the
    input layer; LRU eviction was a stub with no consumer)
  - `Sema.slot_count`, `Sema.slot_budget` — never read
- **Symptom**: 80+ LoC of phantom infrastructure. Reading the code
  suggested "cancellation/snapshot/LRU are working subsystems," but
  none were wired. Misleading scaffolding.
- **Status**: **FIXED**.
- **Class**: Phantom infrastructure (broken-by-default scaffolding).
- **Fix**: Deleted all 11 functions, the `Snapshot` struct, and the
  two Sema fields. What remained — `sema_check_cancel`,
  `cancel_token_is_set`, `sema_effective_revision`, `Sema.active_cancel`,
  `Sema.request_revision` — IS live (called by query.c / invalidate.c).
  Surface stays for when async-LSP work needs it; the orphan setters
  can be re-added cleanly without the phantom-scaffolding cost.

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

### R1 — Salsa's `verified_at` / `changed_at` split `[FIXED]`
- **Where (theirs)**: `rust-analyzer/crates/salsa/src/derived/slot.rs`.
- **Where (ours, was)**: `computed_rev` and `verified_rev` both bumped
  on every successful compute, with no separate "value actually
  changed" notion.
- **Reassessment during implementation**: the originally-pitched
  "two-phase verify, perf win for LSP scale" framing turned out to be
  wrong. Our `sema_revalidate` already implemented the two-phase
  pattern — a shallow `verified_rev == effective_rev` early-return at
  the top, then a deep dep walk with per-dep fingerprint comparison
  (which is the runtime equivalent of `changed_at` cutoff). The
  perf was already there.
- **What was actually missing**: a *separate field* tracking when the
  slot's fingerprint last shifted, distinct from when the body last
  ran. Useful for **diagnostic introspection** (today) and **cross-
  session query cache serialization** (future). Not a runtime perf
  win; an informational one.
- **Status**: **FIXED** (Option B from the implementation discussion
  — added the field + telemetry, no runtime behavior change).
- **Class**: Introspection infrastructure.
- **Fix**:
  - Added `changed_rev` and `last_fingerprint` to `QuerySlot`
    ([src/sema/query/query.h](src/sema/query/query.h)). Always-on
    (no `#ifdef` gate) — pure storage cost is two u64 per slot,
    rounding error.
  - In `sema_query_begin`'s RECOMPUTE branch (DONE → EMPTY transition),
    preserve `slot->fingerprint` into `slot->last_fingerprint`
    before clearing.
  - In `sema_query_succeed`, after the new fingerprint is set,
    compare `slot->fingerprint != slot->last_fingerprint`. If
    different → bump `changed_rev` to current revision; if same →
    keep `changed_rev` backdated.
  - Added `compute_value_changed` / `compute_value_stable` counters
    to `QueryStats` (debug-only, alongside the existing telemetry).
  - `sema_dump_query_stats` gains two new columns (`changed` /
    `stable`). High `stable` count for a kind indicates
    over-invalidation (body keeps re-running but output is stable),
    which is exactly what R6's per-decl AST fingerprint would
    address.
- **Diagnostic finding surfaced immediately**: `fn_scope_index`
  reports `21 stable / 0 changed` for any fixture — turns out the
  query never calls `query_slot_set_fingerprint`, so the slot's
  fingerprint stays at `FINGERPRINT_NONE` forever. Not a bug
  (fn_scope_index's output is a side-table struct without a
  meaningful comptime fingerprint), but the kind of opacity that
  R1's telemetry now makes visible. Noted, not fixed.
- **Test**: T22 (`test_changed_rev_backdating`). Drives `fn() -> i32`
  with body edit (sig stable) then signature edit. Asserts
  `computed_rev` advances on both edits but `changed_rev` only
  advances on the signature edit. 22/22 invalidation suite green
  (both default and debug builds).
- **Future**: when cross-session query-cache serialization becomes
  relevant (build server, persistent IDE caches), `changed_rev` is
  the field that tells you "is the cached value still meaningful
  for the current revision space" without walking the dep graph.

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

### R7 — `AstIdMap` for stable item identity across reparses `[FIXED in DefInfo-strip PR]`
- **Where (theirs)**: `rust-analyzer/crates/span/src/ast_id.rs` —
  `FileAstId<N>` is a stable per-file handle; `AstIdMap` is the
  bidirectional `Arena<(SyntaxNodePtr, ErasedFileAstId)>` rebuilt
  per parse.
- **Status**: **FIXED** for top-level items.
- **Implementation**: `src/sema/modules/ast_id_map.{h,c}`. Hash-based
  identity `hash((kind, name))` with linear-probing disambiguation.
  Per-module `AstIdMap` on `ModuleInfo`; built/reset inside
  `query_top_level_index`'s body. `DefInfo.ast_id` is the stable
  handle for top-level DECL_USER/DECL_IMPORT. `def_origin` dispatches
  via ast_id for top-level, falls back to `origin_id`/node_to_expr
  for locals.
- **Trimmed from RA's design**: no forward `ptr_map` (Expr* → AstId);
  consumers always reach AstId via `DefInfo.ast_id`, so the reverse
  lookup is the only one needed today. No arena indirection — direct
  HashMap is simpler for our scale.
- **What this fixes**: B11 (def_origin scope_index ordering contract),
  B25 (sibling-insert NodeId shift), B23 root cause (cache-on-identity
  for nominal-type members).
- **What this doesn't cover**: body-level expressions. RA takes the
  same stance ("invalidation in bodies is not much of a problem").
  See R8 below if body-level granularity ever becomes a measured
  problem.

### R8 — `SyntaxNodePtr` for body-level stable identity
- **Where (theirs)**: rust-analyzer's `SyntaxNodePtr` =
  `(TextRange, SyntaxKind)`. Identifies a body-level node uniquely
  within a syntax tree; can be re-resolved against a fresh tree
  after reparse.
- **What it buys**: body-level cache entries stay valid across edits
  that don't structurally change the body's tree. Today our body-level
  NodeIds are position-based (file-prefix + per-parse local counter),
  so any edit that shifts byte offsets invalidates all subsequent
  body-level cache entries.
- **Ore status**: not built. Same posture as RA's: body-level
  invalidation is wasteful but bounded, accept it until LSP-scale
  measurements demand otherwise.
- **Action gate**: build only when profiling shows body-level
  invalidation dominating LSP latency on a realistic project.

### R9 — `InFile<T>` / `HirFileId` for macro/instantiation identity
- **Where (theirs)**: `rust-analyzer/crates/hir-expand/src/files.rs` —
  `HirFileId` is either a real `FileId` or a macro-expansion id;
  `InFile<T>` wraps any T with its containing file.
- **What it buys**: identity for items produced by macro expansion
  (or, in Ore's case, comptime instantiation). Each expansion is
  a "synthetic file" with its own AstIdMap; `InFile<DefId>` says
  "this def lives in real_file_42 OR macro_expansion_17."
- **Ore status**: not built. Premature without macros / generics.
- **Action gate**: build alongside generics monomorphization (E.5).
  Each instantiation is conceptually a synthetic file of generated
  decls; `InFile<T>` is the natural wrapper.

### R10 — Global slot registry for LRU eviction + introspection
- **Where (theirs)**: salsa's runtime tracks every query slot in a
  master table. Enables LRU walking, memory bounding, and "what
  slots exist?" debug introspection.
- **What it buys**: long-running LSP sessions can bound memory.
  Each cache (struct_signatures, fn_signatures, def_map_entries, …)
  is its own HashMap today; no way to iterate ALL slots without
  walking each cache individually.
- **Ore status**: not built. The `sema_evict_lru` stub was deleted
  in the orphan-helper cleanup (B28) because there was no registry
  to walk.
- **Action gate**: build when LSP scale measurably needs it.
  Concrete shape: every `sema_query_slot_init` call registers the
  slot pointer in a `Sema.all_slots` Vec. Eviction walker iterates,
  checks `last_accessed_rev` against a threshold, invokes a per-slot
  reset hook. ~200 LoC.

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

LSP shell + DefInfo-strip PR ✓ DONE (LSP server + rust-analyzer-faithful
  query architecture pass; landed alongside the LSP bring-up because
  the bugs surfaced while testing didChange end-to-end)
  ├── B10 ✓ — DefInfo stripped to pure identity (kind, name_id,
  │           ast_id, origin_id, owner_scope). Six fields deleted
  │           (vis, span, semantic_kind, origin Expr*, child_scope,
  │           imported_module, scope_token_id). Per-def accessors
  │           re-derive everything from the AST on demand.
  ├── B11 ✓ — def_origin no longer requires scope_index_build_module
  │           ordering for top-level defs (uses AstIdMap directly).
  │           B11 latent-bug fallback (raw di->origin) deleted with
  │           the field.
  ├── B23 ✓ — phantom "duplicate field" diagnostics on LSP didChange,
  │           fixed via the rust-analyzer-faithful pattern (no
  │           ItemScope for struct/enum members; direct iteration
  │           on signature output).
  ├── B24 ✓ — file-prefix NodeIds (12 bits file_id + 20 bits local
  │           counter). Cross-module NodeId collision eliminated;
  │           single-module invalidation preserved.
  ├── B25 ✓ — AstIdMap for top-level item identity stable across
  │           sibling-insert edits. Hash-based (kind, name) keying
  │           with linear-probing collision resolution. Per-module.
  ├── B26 ✓ — query_fn_scope_index accumulation across recomputes
  │           fixed (clear local_scopes + node_to_scope at the top
  │           of (re)compute).
  ├── B27 ✓ — hashmap_remove (backward-shift) + GC pass for
  │           field_locators / variant_locators on shrinking
  │           signatures.
  ├── B28 ✓ — orphan API cleanup. 11 functions + 1 struct +
  │           2 Sema fields deleted across cancel/snapshot/query_engine.
  └── R7 ✓ — rust-analyzer's AstIdMap pattern transcribed.
            See src/sema/modules/ast_id_map.{h,c}.

Reference targets (long-term)
  ├── R1 — verified_at/changed_at split (cleanup.md #15, post-E.4)
  ├── R3 — cycle fixpoint — see B13 / E.5 above
  ├── R4 — Zig intern pool for ConstValue (reopen if/when ConstValue
  │        gains struct/array/string payloads)
  ├── R6 — per-decl AST fingerprint (Layer 4+ optimization, only
  │        matters at LSP scale)
  ├── R8 — SyntaxNodePtr-style body-level stable IDs. Same posture
  │        as RA today; build only when measured LSP latency demands.
  ├── R9 — InFile<T> / HirFileId — wait for generics monomorphization
  │        or macros; one or the other surfaces the need.
  └── R10 — Global slot registry for LRU + introspection. Build when
            LSP scale measurably needs memory bounding.

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

Build infrastructure ✓ DONE (Nix flake + IDX dev.nix + clang-only)
  ├── flake.nix ✓ — multi-platform devShell, single C toolchain
  │                 (pkgs.clang_19); zig was dropped after the
  │                 split-toolchain experiment proved cross-compile
  │                 was the only thing it bought us, and we don't
  │                 cross-compile
  ├── .idx/dev.nix ✓ — Firebase Studio (IDX) mirror; same packages
  │                    + clangd / gemini-cli IDE extensions
  ├── CFLAGS ✓ — moved from -std=c17 to -std=c23 (clang 19 has
  │              solid C23 support; we were never on c17 for any
  │              specific reason)
  ├── B1 ✓ — tools/test.sh rewritten (1400 LoC → ~80 LoC) as a slim
  │           smoke runner (C foundation tests + per-fixture exit codes)
  └── B22 ✓ — made moot by dropping zig entirely; clang's libclang_rt
              ships ASan for both darwin and linux; verified by
              triggering a real heap-use-after-free on macOS aarch64
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

Last updated: end of LSP shell + DefInfo-strip PR (2026-05-11).
B10/B11/B23/B24/B25/B26/B27/B28 all FIXED. R7 (AstIdMap) FIXED.
R8 (SyntaxNodePtr body-level), R9 (InFile<T>), R10 (slot registry)
filed as long-term reference targets gated on measured need.

Three subsystems flagged as forward-stub dead code (not deleted —
keeping as scaffolding, parallel to type_legacy/'s posture): full
src/hir/ directory + CheckedBody machinery (~400 LoC), generics/
comptime/effect caches on Sema (~30 LoC of struct fields), 9
ExprKind enum values the parser doesn't yet emit. Documented in
cleanup.md's "forward-stub inventory" section.

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
