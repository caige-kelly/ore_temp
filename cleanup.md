# cleanup.md — current state, what's done, what's next

Living checklist of architectural cleanups. Updated 2026-05-11 after
the LSP shell + DefInfo-strip session. Pairs with `bug_of_bugs.md`
(which tracks discovered bugs by `B`/`F`/`R` number); this doc tracks
broader cleanup work and lists clear next steps.

When a cleanup.md item maps to bug_of_bugs entries, it's annotated
inline `(see bug_of_bugs B10, B11)`.

---

## ✅ Closed since the original audit

Original cleanup.md had 24 numbered items. The vast majority shipped
across PR 1 → PR 4 → PR 3.5 → the #16 audit → LSP-shell PR.

**Architectural correctness blockers (#1-#5 in the original list):**

1. ✅ **Incremental tests** — 25 in `tools/sema_invalidation_test.c`,
   plus per-fixture exit-code coverage and a determinism harness. Way
   past the "5 minimum" the original audit asked for. *(see bug_of_bugs F1-F11, B23-B28)*
2. ✅ **`DefInfo.origin` removal** — the entire field is gone. DefInfo
   is now `{kind, name_id, ast_id, origin_id, owner_scope}` — pure
   identity. *(see B10, B11)*
3. ✅ **`is_comptime_evaluable` as a query** — `query_is_comptime`
   with its own slot + fingerprint + AST dep. *(F10)*
4. ✅ **`query_const_eval` complete enough** — handles `Ident`,
   `Builtin` (primitives), `If`, `Switch`, `Block`, all arith ops,
   bool literals via `CONST_BOOL`. *(F11-F13)*
5. ✅ **Coerce variance rules** — `^T → ^const T`, `[]T → []const T`,
   `^[N]T → []T`, `^[N]T → [^]T`, `nil → ?T / ^T / []T`, `noreturn →
   anything`. *(B15, coerce.c branches verified)*

**Type-system known partials (#6-#10):**

6. ✅ **`?T` optional type** — `TY_OPTIONAL` + nil coercion + `?`
   postfix unwrap + `orelse` *(B15, B16)*
7. ✅ **`string` as `[]const u8`** — `s->string_type = type_slice(s,
   u8_type, /*is_const=*/true)`. Slice interop works.
8. ✅ **`Fn` in type position** — capital-`Fn` keyword + `expr_FnType`
   case in resolve_type_expr. Lambda is value-only. *(B17/F15)*
9. ✅ **`[^]T` pointer arithmetic** — `+/- int`, `[^]T - [^]T → usize`,
   pointer equality. *(B18)*
10. ✅ **Bidirectional through if/switch** — `check_expr(branch,
    expected)` propagates through both branches.

**Resolution / scope (#11-#14):**

11. ✅ **Nested struct-literal bidirectional** — confirmed working;
    `check_expr(field.value, sig->fields[idx].type)` is the path.
12. ⏸️ **`expr_DestructureBind` typing** — STILL OPEN. Parser emits
    them (per ast.h's enum) but sema falls through to error. Common
    pattern but not yet exercised by any fixture.
13. ✅ **Path resolution exercised** — `query_resolve_path` /
    `resolve_member_lookup` now dispatch-by-kind for struct/enum
    members (rust-analyzer pattern). *(B23)*
14. ✅ **Field DefIds load-bearing** — used by `struct_find_field_def`,
    `field_locators`, GC pass on shrinking signatures. *(B27)*

**Salsa-faithful (#15-#17):**

15. ✅ **`verified_at` / `changed_at` split** — `changed_rev` +
    `last_fingerprint` on `QuerySlot`. *(R1)*
16. ✅ **Untracked-read detection** — `SEMA_READ_UNTRACKED` macro +
    `has_untracked_read` flag + `REVALIDATE_RECOMPUTE` branch.
    Audit found 0 sites needed wrapping today. *(R2)*
17. ⏸️ **Cycle fixpoint recovery** — STILL DEFERRED. Required for
    generics (E.5). Today's sentinel-on-cycle is fine for nominal
    types. *(B13)*

**Hygiene / dead code (#21-#24):**

21. ⏸️ **Product-literal parse duplication** — `.{...}`, `Point{...}`,
    `[N]T{...}` still have three copies. Not investigated this
    session.
22. ⏸️ **Diagnostic codes + phrasing** — STILL DEFERRED. Error
    messages inconsistent; `E0100`-style codes referenced in
    comments but not wired.
23. ✅ **`Sema.name_*` pre-interned names wired** — `name_sizeOf`,
    `name_alignOf`, `name_TypeOf`, `name_intCast`, `name_typeName`
    initialized in `sema_init`. *(F14)*
24. ⚠️ **Sema fields declared but unused** — partially done. F14
    cleaned several (`anytype_type`, `effect_row_type`,
    `scope_token_type`, `effect_type`). New audit (this session)
    found more — see "forward-stub inventory" below.

---

## Newly closed in the LSP-shell + DefInfo-strip session

These weren't in the original cleanup.md — they surfaced during
the LSP bring-up and the rust-analyzer-faithful query architecture
pass.

**Identity-record stripping:**

- ✅ DefInfo collapsed from 10 fields to 5. `vis`/`span`/`origin`
  Expr*/`semantic_kind`/`child_scope`/`imported_module`/`scope_token_id`
  all deleted and replaced with per-def accessors. *(B10)*
- ✅ `AstId` + per-module `AstIdMap` implemented for top-level
  identity stability across reparses. *(R7, B25)*
- ✅ Cross-module `NodeId` collision eliminated via file-prefix
  encoding (12 bits file_id, 20 bits local counter). *(B24)*
- ✅ `child_scope` concept deleted for struct/enum members;
  member lookup is direct iteration on signature output (RA's
  no-`ItemScope`-for-fields pattern). *(B23)*

**Output-state hygiene:**

- ✅ `query_fn_scope_index` cleared on recompute. *(B26)*
- ✅ `hashmap_remove` with backward-shift cleanup added; orphan
  field/variant locators GC'd on signature shrink. *(B27)*

**Phantom API surface:**

- ✅ 11 orphan helpers deleted across cancel/snapshot/query_engine
  + 2 dead Sema fields (`slot_count`, `slot_budget`). *(B28)*

**LSP shell:**

- ✅ `ore lsp` subcommand + JSON-RPC stdio loop.
- ✅ `OreDb` wrapping Sema with per-input `Draft` tracking.
- ✅ `textDocument/didOpen` / `didChange` / `didClose` /
  `publishDiagnostics`.
- ✅ `vscode-ore` extension with TextMate grammar + LanguageClient.
- ✅ 146/146 tests green; LSP edit-cycle smoke clean.

---

## 🟢 Open items, ranked by readiness-to-act

### Small + actionable now

12. ⏸️ **`expr_DestructureBind` typing** — `(a, b) := pair`. Parser
    emits the node; sema rejects. Real users want this. *Cost: ~150
    LoC; gated on having a pattern-walker decision.*

11. ⏸️ **Product-literal parse duplication** — three copies of the
    field-list parser. Extract a helper. *Cost: ~50 LoC refactor.*

22. ⏸️ **Diagnostic codes (E0100-style)** — wire the codes referenced
    in comments. Future LSP integration will want them for client-side
    severity / quick-fix. *Cost: ~100 LoC + per-diagnostic
    classification pass.*

### Medium + gated on feature work

17. ⏸️ **Cycle fixpoint recovery** — needed for E.5 (generics). Today
    sentinel-on-cycle works because nominal-type identity sidesteps
    self-reference. *(B13)* — natural in the E.5 PR.

18. ⏸️ **Effects rebuild** — `lambda.effect` AST exists but unused.
    `effect_ops.c` is a stub. Requires `query_effect_signature`,
    effect propagation in fn signatures, handler scope tracking.
    The big one. *Cost: ~1000-2000 LoC.* — own PR series.

19. ⏸️ **Pattern matching** — tuple/struct destructuring, range
    patterns, guards, captures, decision-tree compilation.
    *Cost: ~1500 LoC.* — own PR series.

### Large + gated on measured need

R8. **SyntaxNodePtr-style body-level stable IDs** — same gap as
    rust-analyzer ("invalidation in bodies is not much of a
    problem"). Build only when body-level invalidation dominates
    measured LSP latency. *Cost: ~400-600 LoC.*

R10. **Global slot registry for LRU + introspection** — the one
    purely-query-system gap remaining. Required for memory-bounded
    long-running LSP sessions. *Cost: ~200 LoC.* Concrete shape
    documented in bug_of_bugs's R10 entry.

R9. **`InFile<T>` / `HirFileId` for macros/generics** — needed
    when comptime instantiation lands (generics monomorphization).
    Each instantiation is a "synthetic file" with its own AstIdMap.
    *Cost: ~300 LoC.* Co-design with E.5.

---

## ⚠️ Forward-stub inventory (kept as scaffolding, not deleted)

These are scaffolded for features that aren't built. Same posture
as `type_legacy/` — keep as design reference, delete the day a
feature gives us a more concrete shape.

Documented here so future-you doesn't mistake them for working code.

### Subsystem #1: HIR / CheckedBody (~400 LoC)

- `src/hir/` directory: `hir.{c,h}`, `dump.{c,h}` — built into binary,
  zero external callers.
- `CheckedBody` struct + `current_body` field on Sema: zero readers.
- `bodies` Vec / `bodies_table` Vec / `BodyId` / `body_info` / etc.:
  no producers.
- `module_hir` / `decl_hir` HashMaps: never populated.

**Purpose**: HIR lowering for codegen. Won't materialize until codegen
work starts (post-E.5). The design will likely change substantially —
keeping today's stub means preserving an outdated plan.

**Action gate**: when codegen begins, delete and re-add with the
shape the codegen pass actually needs.

### Subsystem #2: Generics / comptime / effect caches (~30 LoC of fields)

- `Sema.instantiations` (Vec) + `instantiation_buckets` (HashMap):
  generics specialization. Zero refs.
- `Sema.call_cache`: comptime call memoization. Zero refs.
- `Sema.effect_sig_cache`: effect signature interning. Zero refs.
- `Sema.comptime_call_depth` + `Sema.comptime_body_evals`: depth
  guard + instrumentation. Zero refs.

**Purpose**: future generics (E.5) + comptime + effects work.

**Action gate**: when generics begins, decide if these field shapes
match the actual design. Re-add if they do; delete if not.

### Subsystem #3: Parser ExprKinds without producers

[src/parser/ast.h](src/parser/ast.h) declares ExprKind values that
the parser doesn't emit. Each has switch-case handlers in dump.c,
layout.c, expr_check.c, etc. returning sane defaults.

- `expr_DestructureBind` — gated on cleanup item #12 above
- `decl_Effect`, `expr_EffectRow` — gated on effects rebuild (#18)
- `expr_Handler`, `expr_Mask` — same
- `expr_Ctl` — control / continuation; effects-related
- `expr_EnumRef` — duplicate / replaced by `expr_Field` path
- `expr_Asm` — inline assembly
- `expr_ArrayType` — duplicate / replaced by other type-expr forms

**Purpose**: forward intent. When the parser eventually emits one,
the compile-error-on-missing-case behavior across every switch
forces the implementer to handle it everywhere.

**Action gate**: leave alone. The "compiler tells you what's missing"
property is load-bearing for incremental development.

---

## 📝 Recurring lessons (from this session and the prior PRs)

1. **Identity records must not cache derived state.** B10, B11, B23,
   B26 are all the same bug class — putting "AST-derived" or
   "computed" data on a struct that persists across revisions. The
   fix shape is always the same: strip the identity record to pure
   identity, move derived data to query outputs that re-derive on
   demand. Rust-analyzer's `*Loc` / `*Id` types are the reference.

2. **Forward-stub scaffolding has value, but only if labeled.**
   `type_legacy/` works because the Makefile prunes it and the
   comment block says "design reference." The HIR/instantiation/
   ExprKind stubs in the LIVE codebase weren't labeled — they read
   like working subsystems. The fix isn't to delete them, it's to
   document them as forward stubs (this section).

3. **Write the test before the fix.** B3/B4/B5/B12 were filed
   speculatively before PR 1's reuse-path fix had been understood.
   The "edit-cycle hygiene PR" turned out to be a docstring update
   after T16/T17/T18 proved the concern away. Pattern recurs in
   every PR — write the failing test first.

4. **Telemetry pays for itself immediately.** B14's stat dump
   surfaced B20 the first time it ran. Worth instrumenting before
   you think you have a problem.

5. **Position-based identity is fragile but bounded.** rust-analyzer
   addresses item-level identity with `FileAstId<T>` (name-keyed,
   stable across reparses) and accepts position-based identity
   for body-level expressions. Ore now matches that posture (AstIdMap
   for top-level, position-based with file-prefix for body NodeIds).

---

## Next steps (concrete, in order)

If you want a one-PR-at-a-time path forward:

1. **`expr_DestructureBind` typing** (#12) — small, real users want
   this, unblocks tuple/struct patterns.

2. **Diagnostic codes** (#22) — small, sets up better LSP UX.

3. **Effects rebuild** (#18) — big, but everything else is gated on
   it being either built or deliberately deferred. After this, the
   `decl_Effect` / `expr_Handler` / `expr_Mask` / `expr_EffectRow`
   forward stubs collapse into live code.

4. **Generics (E.5)** — co-design with `InFile<T>` (R9) and cycle
   fixpoint (#17). When this lands, the `instantiations` / `call_cache`
   / `effect_sig_cache` Sema fields become live.

5. **R10 slot registry** — build when LSP scale measurably needs it.
   Not blocked on language features; can land any time but only
   pays off at scale.

6. **HIR / codegen** — far future. The src/hir/ stubs go away or
   get reborn here.
