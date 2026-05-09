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

### B1 — `tools/test.sh` is broken
- **Where**: [tools/test.sh:3](tools/test.sh#L3) — header comment literally says
  `## TODO: fix. broken after sema refactor`.
- **Symptom**: `make test` fails immediately at the arena smoke-test build with
  `undefined symbol: ___asan_*` linker errors. Running `zig cc` on macOS doesn't
  link the right ASan runtime.
- **Root cause**: The script hardcodes `-fsanitize=address` + `-lasan`; macOS
  needs the ASan runtime to be picked up via the toolchain, not via `-lasan`.
- **Status**: **OPEN** (deferred during PR 1).
- **Class**: Build-config drift.
- **Action**: Either drop `-lasan` and rely on `-fsanitize=address` alone, or
  branch on `$(uname -s)`. Validate `make test` runs end-to-end after.

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

### B3 — `node_to_expr` is monotonically growing across re-parses
- **Where**: [src/sema/resolve/scope_index.c:55-65](src/sema/resolve/scope_index.c#L55-L65)
  — `record_node_expr` does `hashmap_put` keyed by `NodeId`. Same NodeId from a
  later parse overwrites the prior `Expr*`. NodeIds present in the prior parse
  but absent in the new one stay as orphaned entries.
- **Symptom**: After many edits, `node_to_expr` has dead pointers under
  no-longer-valid keys. `def_origin` for a stale `origin_id` returns the
  orphan. Today nothing reads the orphans (DefIds whose origin_id is stale are
  themselves orphaned, see B5), but it's a correctness landmine for any future
  code that walks `node_to_expr` directly.
- **Root cause**: No GC or generation-tagging on the map.
- **Status**: **OPEN**, latent.
- **Class**: Hand-managed cache.
- **Action**: Either (a) clear `node_to_expr` at the start of each
  `query_module_ast` recompute and have `scope_index_build_module` repopulate
  it, or (b) tag each entry with a per-parse generation counter and lazily
  invalidate stale entries. (a) is simpler and correct; (b) preserves cross-
  module reuse if we ever load multiple modules in parallel.

### B4 — `top_level_index` recompute leaks `DefMapEntry` for removed names
- **Where**: [src/sema/modules/def_map.c:127-141](src/sema/modules/def_map.c#L127-L141)
  — `get_or_create_entry` keeps every `DefMapEntry` alive in
  `m->def_map_entries`. When source A defines `foo` and source B removes it,
  foo's entry stays in the hashmap with `def = D` pointing at the now-orphaned
  DefId D in `defs_table`.
- **Symptom**: Memory grows by O(removed-decls) per session. Stale DefIds
  remain in `m->internal_scope` (see B5). Doesn't cause incorrect output today
  because resolution chains through scope_define_def's name_index, not the raw
  `defs` Vec — but a duplicate-name scenario (foo removed, then re-added with
  fresh DefId) causes scope corruption.
- **Root cause**: The `def_for_name` slot's ERROR-revalidate path (PR 1 fix)
  recovers the slot but doesn't unwind the prior compute's side effects.
- **Status**: **OPEN**, latent.
- **Class**: Hand-managed cache, accumulator without unrolling.
- **Action**: When a `DefMapEntry`'s slot transitions from DONE to ERROR (name
  removed in this revision), call `scope_undefine_def(internal_scope, D)` and
  clear `entry->def`. Mirrors the `refs_unrecord` accumulator-drop pattern that
  `query_resolve_ref` uses correctly.

### B5 — `defs_table` and `scope.defs` grow monotonically
- **Where**: [src/sema/modules/def_map.c:254](src/sema/modules/def_map.c#L254)
  (`def_create`) — appends to `s->defs_table` with no shrink.
- **Symptom**: Memory bloat over many edit cycles. Orphaned DefIds accumulate.
  Nothing breaks today because all consumers go through scope_define_def's
  name_index, but `sema_def_count(s)` returns inflated numbers and any future
  code that walks `defs_table` directly will see ghosts.
- **Root cause**: Append-only ID tables are correct for stable-ID guarantees
  but require a separate "this DefId is no longer reachable" flag for cleanup.
- **Status**: **OPEN**, latent.
- **Class**: Hand-managed cache.
- **Action**: Add a `dead` flag to `DefInfo` set when `scope_undefine_def`
  fires (see B4). Existing readers stay correct; iteration paths skip dead
  entries.

### B6 — `query_resolve_ref` slot keyed by (NodeId, Namespace) — two lookups per Ident
- **Where**: [src/sema/resolve/resolve.c:106](src/sema/resolve/resolve.c#L106).
  Many call sites (`is_bind_with_value_kind`, `type_of_ident`, etc.) try
  `NS_VALUE` first and fall back to `NS_TYPE` on failure — that's two GUARD
  evaluations + two slot lookups per Ident.
- **Symptom**: Subtle perf regression and dep-graph bloat. Each Ident
  generates two `record_dep_on_parent` calls (one per slot tried).
- **Root cause**: Namespaces leak into the slot key. Resolution is logically
  one operation — "what does this Ident bind to?" — that should pick the
  correct namespace internally based on context (NS_VALUE for value-position
  Idents, NS_TYPE for type-position).
- **Status**: **OPEN**.
- **Class**: API granularity / coarse fingerprint inversion.
- **Action**: Rewrite `query_resolve_ref` to take a `NamespaceContext` (just
  VALUE_OR_TYPE today) and return whichever resolves. Single slot per NodeId.

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

### B12 — `scope_define_def` may collide on rename re-add
- **Where**: [src/sema/modules/def_map.c:263-268](src/sema/modules/def_map.c#L263-L268)
  — `scope_define_def(internal_scope, def)` returns false on duplicate name.
  After rename (B4), the old DefId is still in scope; if the user adds a new
  decl with the same name later, the new def fails to install.
- **Symptom**: Edit `old :: 1` → `new :: 2` → `old :: 3` and `old`'s third-
  revision DefId silently fails to insert because the orphan from revision 1
  is still in scope. The slot returns DEF_ID_INVALID and the user sees
  `'old' not defined` even though their source clearly defines it.
- **Root cause**: Tied to B4 / B5 — the scope's name_index isn't unwound when
  a DefMapEntry transitions to ERROR.
- **Status**: **OPEN**, exact reproduction not yet harnessed.
- **Class**: Accumulator without unrolling.
- **Action**: Fix B4 (unwind on slot-failure transition) and this falls out.
  Add a regression test: T16 — rename then re-add original name.

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

### B18 — `[^]T - [^]T` pointer difference deferred
- **Where**: [src/sema/type/expr_check.c:264-273](src/sema/type/expr_check.c#L264-L273) —
  the many-pointer arith branch handles `[^]T +/- int` only. Two
  many-pointers subtracted produce nothing (falls into the diagnostic).
- **Symptom**: `(end - begin)` to compute span length errors with
  "pointer arithmetic '-' on [^]T with [^]T is not supported."
- **Root cause**: Defining the result type (`isize`? `usize`?) and
  semantics (must be same elem type? alignment-aware?) needs a
  considered design decision.
- **Status**: **OPEN**, low priority.
- **Class**: Incomplete operator surface.
- **Action**: Decide: result is `isize` (signed difference); operands
  must have the same elem type and same constness (or both must be
  the same many-ptr type). Add the case to `bin_arith_result` Minus
  branch + fixture.

### B19 — `coerce`'s `emit` flag is a code smell
- **Where**: [src/sema/type/coerce.c:97-110](src/sema/type/coerce.c#L97-L110) —
  `coerce_check(emit)` was added in PR 3 #6 to let the optional-lift
  rule speculatively recurse without producing duplicate diagnostics.
- **Symptom**: Every future variance rule that wants speculative
  recursion needs to thread the same flag. Diagnostic emission is
  entangled with structural decision-making.
- **Root cause**: The clean separation would be a pure
  `coerce_structural_ok(from, to, value) → bool` predicate (no
  emission, no Sema needed for the structural check) and a thin
  `coerce(s, from, to, value, span)` wrapper that calls the predicate
  then emits a single diagnostic on failure with the right context.
- **Status**: **OPEN**, low priority — current shape works.
- **Class**: Code smell.
- **Action**: Refactor when adding the next speculative-coerce rule
  (likely the typed-nil branch in B15). Keep the public API stable.

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

Edit-cycle hygiene PR (NEW; not in original cleanup.md taxonomy)
  ├── B4 — DefMapEntry unwind on slot DONE→ERROR (name removed)
  ├── B5 — DefInfo `dead` flag; iteration skips dead entries
  ├── B3 — node_to_expr clear-or-generation-tag on re-parse
  ├── B12 — rename re-add scope collision (falls out of B4)
  └── Test: T16 — rename then re-add original name
  Must land before LSP shell work; LSP hammers these edit paths.

#16 follow-up PR (untracked-read trace mode + query observability)
  ├── ~585 SEMA_READ migrations (the original #16 spec)
  ├── B8 — ast_dep helper consolidation into src/sema/query/ast_dep.{h,c}
  ├── B9 — query_def_for_name slot fingerprint (was always FINGERPRINT_NONE)
  ├── B11 — assert node_to_expr is current-revision when def_origin is read
  └── B14 — per-QueryKind telemetry under same #ifdef ORE_DEBUG_QUERIES

#22 follow-up PR (diagnostic codes + phrasing)
  └── No bug_of_bugs entries map directly. Optional: + B1 if lumping tooling.

PR 4 (resolution / scope cleanup, original cleanup.md #11-#14)
  ├── + B6 — query_resolve_ref unification (two-lookup-per-Ident perf fix)
  ├── + B17 — anonymous user-types in fn-type-position params (parser
  │           relaxation to make `fn(Point, Color)` work without `name:`)
  ├── + B18 — `[^]T - [^]T` pointer difference (op completeness)
  └── + B19 — coerce `emit` flag refactor (split structural / emit)

E.5 (generics)
  └── B13 — cycle fixpoint recovery (already cleanup.md #17)

Reference targets (R1-R6) — each its own design PR
  ├── R1 — verified_at/changed_at split (cleanup.md #15, post-E.4)
  ├── R2 — untracked-read detection — see #16 follow-up above
  ├── R3 — cycle fixpoint — see B13 / E.5 above
  ├── R4 — Zig intern pool for ConstValue (now in PR 3.5 above)
  ├── R5 — abiSize for layout (now in PR 3.5 above)
  └── R6 — per-decl AST fingerprint (Layer 4+ optimization)

Standalone (no natural PR home yet)
  └── B1 — tools/test.sh ASan repair (~30 LoC, run any time)
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

Last updated: end of PR 3.5. B15 (nil), B16 (optional unwrap), and
R5 (aggregate layout) all moved to fixed. PR routing table updated
to mark PR 3.5 done. R4 (Zig intern pool for ConstValue) re-deferred
since none of B15/B16/R5 needed it. Edit-cycle hygiene PR is now
the next gating item.
