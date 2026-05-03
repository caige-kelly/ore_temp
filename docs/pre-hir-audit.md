# Pre-HIR Audit & Cleanup

## Why this exists

We're going to introduce HIR. HIR amplifies whatever's underneath it — clean
foundation pays back, muddy foundation bleeds. This doc tracks what's
still open: deferred architectural work, settled decisions worth
recording so we don't relitigate them, and properties HIR will depend on
that need to stay honored.

The completed audit findings (Passes 1-6, file-by-file inventory of
`Decl` / `SemaDeclInfo` / walkers / queries / AST mutations / fact
recording / memory ownership) are in the git history.

---

## What HIR replaces (skip these — don't audit further)

- `CheckedBody`'s shape (facts-keyed-by-`Expr*`). HIR replaces with
  typed instructions; the hashmap structure is borrowed time.
- The current shape of `sema_infer_expr`'s big switch returning a
  `Type*`. After HIR, the equivalent emits HIR ops + assigns types as
  part of emission.
- The const_eval interface (already audited).
- AST mutation paths the splicer would add.

---

## Open deferred work

Surfaced during cleanup but explicitly punted — the HIR transition (or
later) is the natural moment to address them.

### Comptime builtins should route through the query system

`@sizeOf`, `@alignOf`, `@returnType`, `@TypeOf`, `@target.*` are
conceptually queries ("compute X about Y at compile time, cache the
result, detect cycles"), but today they're ad-hoc switch arms in
`eval_builtin` (const_eval.c) and `expr_Builtin` (checker.c). They
delegate to real queries underneath (`sema_layout_of_type_at`,
`sema_infer_expr`), so the heavy work IS cached — but the wrapping
builtin layer isn't query-shaped, has no per-call-site cache, and no
uniform "what comptime computations happened?" view. Adding a builtin
currently touches three files.

Orthogonal to HIR lowering — could ride along in any HIR phase if
convenient, but explicitly out of scope per the HIR plan. Defer until
incremental-rebuild requirements force the issue.

### Real escape/borrow analysis (Phase 4 v1 successor)

Today the RAII safety net only catches the bound name appearing at the
body's tail position (with one level of `&`). Aliasing through
let-binds, struct contents, closure capture, and multi-level refs all
slip through. The full framework would tag values with their
originating scope token and propagate the tag through expressions;
escape from the originating scope is rejected. Likely a
Datalog/dataflow pass. Big design + multi-week build. Punt until
concrete user demand.

When this lands, the escape-heuristic items below collapse into it.

### `with`-sugar escape-check is over-broad

`from_with_sugar` flag + tail-position binder check fires the friendly
"with-bound name 'X' cannot escape its with-block" diagnostic. False
positives like `with x := pure_int_giver; x` (no region semantics) get
flagged. Lower priority now that const-correctness gives users a real
way to express read-only views; the friendly diagnostic is still a net
win until borrow analysis subsumes it.

### Resolver action-shape propagation hack (dissolved by HIR Phase E)

`case expr_Call:` in name_resolution.c does two things that don't
belong in name resolution:

1. **Mutates AST lambda slots.** Writes
   `arg->lambda.{effect, ret_type, params[i].type_ann}` from the
   callee's signature when those slots are NULL. Idempotent and
   contained, but it's the only resolver-side AST mutation that goes
   beyond `Identifier.resolved`-style back-links.
2. **Only inspects direct Lambda bindings.** Triggers only when
   `callee_decl->node->bind.value->kind == expr_Lambda`. Callees
   resolved through field access, parameters, or call results miss
   the propagation entirely.

Both stem from doing expected-type-driven work pre-sema. **HIR Phase E**
moves this to lowering time, where every callee shape has its type
known and the resolver mutation goes away by construction. Until then,
the hack covers the common case (top-level lambda bindings); higher-
order callees with action-typed params are unsupported.

---

## Settled decisions (recorded so we don't relitigate)

Architectural questions that were considered, decided, and merged.
Listed here as "do not reopen without new evidence" markers.

- **`scoped effect<s>` syntax dropped.** `scoped effect` alone declares
  the per-instance scope identity; the action's signature names the
  scope explicitly via `comptime s: Scope`. Parser rejects the legacy
  `<s>` form with a migration error.
- **`HandlerExpr` unified across `handler {…}` (value form) and
  `handle (target) {…}` (apply-to-target form).** A nullable `target`
  field discriminates. Splitting into two AST kinds is a possible
  future cosmetic change if the unified shape causes confusion.
- **`HandlerExpr.effect_decl` lives on the AST node** as a resolver-pass
  back-link (matching `Identifier.resolved`). Removing it would force
  per-read recomputation of the set-equality match (and re-emit
  diagnostics) or a separate sema-side cache. Not worth it.
- **`with_imports` overlay replaced with `DECL_EFFECT_OP` synthetic decls.**
  Effect ops are first-class lexical bindings injected into body
  scopes; lookup is pure scope-chain walk; op-name collisions become
  standard duplicate-decl errors via `decl_new`.
- **`expr_With` narrowed to handler-install only.** Non-handler
  `with caller body` desugars in the parser to `caller(fn(<binder?>) body)`;
  `WithExpr.caller` is `HandlerExpr*`. Three subsystems lose their
  With-classification logic; AST kind alone discriminates handler-
  install from call-sugar.
- **`WithExpr` and `HandlerExpr` stay as separate AST nodes — do not
  re-fold.** `with` is a verb (introduces install context: binder,
  body); `handler {…}` is a noun (a `HandlerOf<E,R>` value). Folding
  forces every walker to branch on `body == NULL` to discriminate
  value-position from install-position handlers — recreating the
  discrimination we just removed.
- **`infer_scope_param` uses match-by-annotation.** The `s` param of
  `fn(comptime s: Scope, action: fn() <Allocator(s)>)` binds to the
  Allocator frame's scope token by walking the callee's signature for
  an effect term that references `s`. Three-tier fallback: match → most
  recent frame → fresh-mint. Composes correctly under nested scoped
  effects.
- **Const-correctness for pointers/slices/arrays.** `Type.is_const`
  flag mirrors `is_optional` — per-Type, threaded through
  constructor/equality/assignability/display. `unary_Const` sets the
  bit; mutation through const l-values is rejected;
  `T → const T` allowed (drop write capability),
  `const T → T` rejected. Transitive through field access on
  const-pointer-to-struct.

---

## What HIR depends on (keep these solid)

These are the foundations HIR will *read from* during lowering. The
audit verified they're in good shape; preserve that.

- **`Decl` shape and lifecycle.** Every field either fully wired or
  deleted. `is_pub` (TODO-gated) and `is_export` (always true)
  acknowledged exceptions.
- **`SemaDeclInfo` as the canonical per-decl sema data.** Sema-time
  info lives here; resolver-time info lives on `Decl`.
- **The query protocol.** `sema_query_begin` / `sema_query_end` used
  uniformly across `TypeOfDecl`, `EffectSig`, `Layout`. HIR adds
  queries.
- **Walker completeness.** Every expression kind handled in every
  walker (`resolve_expr_inner`, `sema_infer_expr`, `sema_check_expr`,
  `validate_expr_identifiers`, `tally_expr`,
  `effect_solver::collect_from_expr`). `-Wswitch` is the safety net.
- **Fact-recording consistency.** Every expression sema visits —
  success or failure — records a fact (failures record
  `s->error_type`).
- **AST is read-only after sema, modulo three documented mutations.**
  Two on the sema side: `expr_Field.field.resolved` in checker.c and
  `expr_EnumRef.name.resolved` in `sema_check_expr` — both gap-fills
  the resolver can't do without expected-type context. One on the
  resolver side: `case expr_Call:` writes
  `arg->lambda.{effect, ret_type, params[i].type_ann}` for action-shape
  propagation when those slots are NULL. Idempotent (only fills NULLs)
  and contained, but it's the same kind of expected-type-driven
  lookup the sema mutations are. All three dissolve under HIR
  lowering, which has type info at the right time.
- **Big-struct ownership is uniformly arena-backed.** No per-struct
  ownership rules to design for HIR.

---

## After this — HIR itself

The HIR transition has its own plan with phases B-H (define structures,
lower AST → HIR with complete coverage, comptime splicing during
lowering, move effect-solver onto HIR, type-directed op visibility,
migrate sema facts, delete the AST-facts path). Each phase is
independently shippable; the codebase compiles and tests pass after
every one. Total scope: 2-3 weeks of focused work.
