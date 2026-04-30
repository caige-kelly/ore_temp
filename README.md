Honest pipeline assessment, source-of-truth–ordered:

Lexer / layout — Mature for v1. Indentation→braces normalizer works. No housekeeping debt here.

Parser — Mature for v1. `forall<s>` keyword removed; scope quantification is now expressed as `comptime s: Scope` and parser auto-promotes those to PARAM_INFERRED_COMPTIME. Three uniform param kinds (RUNTIME / COMPTIME / INFERRED_COMPTIME) flow from the parser through the rest of the pipeline. `pub` keyword threaded through (foundation only — flag is recorded on Bind/DestructureBind, default-export semantics unchanged pending the one-line flip in collect_decl).

Name resolution — Mature. Hashmap-indexed scopes, cycle detection, with-overlay imports, comptime/loop/effect-row depth tracking. `scope_add_decl` is the only sanctioned writer to scope.decls + name_index. Top-level destructures now properly registered (was a silent gap). The handler-impl flag lives on Compiler.handler_impl_decls. with.handled_effect is cached on the AST. No outstanding debt.

Module loader / imports — Works. Cycle detection, path-keyed dedup. Acceptable for v1.

Sema — signature resolution — Solid. compute_decl_signature handles primitives, structs, enums, effects, lambdas, ctl, params, value bindings. Skeleton-then-fill model handles mutual recursion. Sema-only fields (type, effect_sig, body_effects, query slots) live on `Sema.decl_info: HashMap<Decl*, SemaDeclInfo*>`, not on `struct Decl`. The sig-resolution scratch body is documented.

Sema — type checking (expression walk) — Most expression kinds are handled. Real holes:

- ✓ Pointer-deref-then-field-access types correctly via dynamic field lookup against the object's type (`h^.size`).
- Optional unwrap (`?T`, `if (opt) |x|`) is type-flowed casually for the unwrap form. The optional flag is now first-class on `Type`, but the `if (opt) |x|` body doesn't yet strip the flag for the bound name. Comparison-to-nil is OK; explicit unwrap inside if-let needs work.
- `@ptrCast`, `@stackTrace`, `@returnType`, `@typeName`, `@tagName`, `@build` builtins are stubs. Each just returns `unknown` or `comptime_int`. `malloc.ore` uses several of these.
- Array literals' length type isn't tracked. `BINS := [BIN_COUNT]?^Header{nil}` — the `[BIN_COUNT]` doesn't propagate as a type-level constant.
- Defer / orelse / break-from-block expressions typecheck loosely.

Comptime evaluation — Foundation correct, interpreter missing.

ConstValue: int/float/bool/string/type. Works.
ComptimeEnv: chained scopes. Works.
@sizeOf, @alignOf, @target.os/arch/pointer_size. Works.
No interpreter. const_eval_expr does literal folding + ident-from-bind reduction, but it never calls a user function. So comptime fn(x) ... invocations don't actually run. This is the single biggest hole in the comptime story.
No comptime if/switch branch elimination during the body re-walk.
No aggregate ConstValue (struct, array literal). Required for build.ore config tables.

Effect system (Koka-grade target) — Foundation correct, unification missing.

EffectSig (declared) ✅, EffectSet (inferred) ✅, EvidenceVector (per-program-point) ✅.
Sig-vs-body subset check ✅, with-discharge ✅, op-of-effect inference ✅, open-row absorption ✅.
PARAM_INFERRED_COMPTIME for `comptime s: Scope` params, with call-site lookup against the active evidence vector ✅.
Row unification is not implemented. Today solve_effect_rows does subset-check. Real Koka unifies open row variables across call boundaries. Without this, <X | e> doesn't actually flow through the program — e is just a wildcard that absorbs everything locally.
No tail-resumptive inference. Every handler is GENERAL. Codegen will take the slow path (mp_prompt) everywhere.
No multi-shot continuation tracking. ctl ops can resume zero, one, or many times; we don't analyze.
No scope-token escape analysis. scope_token_id is recorded but no checker enforces it. scoped effect<s> values can leak.

Instantiation / specialization — Foundation correct, body re-walk works, signature substitution works. Per-instantiation effect verification runs against the (possibly substituted) declared signature.

Effect rows aren't substituted per instantiation. Same EffectSig* shared across instantiations.
Recursive instantiation rejects, doesn't unfold. List(t) :: struct { tail : List(t) } errors.
No structural type identity. Two instantiations that produce structurally-equal anonymous types are not deduplicated.

Codegen — Doesn't exist yet. The Type*/SemaFact/EvidenceVector shapes are codegen-ready, but no IR, no lowering, no asm/llvm/c emit.

Build system — Doesn't exist. Compiler.target is host-defaulted; no build.ore parsing; no module-graph above the compiler.

Diagnostics — Render layer works. Source map, span-with-file-id, color toggle. Good.

Tests — 51 regression tests + 1 unit test. Decent coverage of what exists.

## Foundation gaps not in line with the stated goals

### ✓ TYPE_NORETURN

Added. Bottom type, assignable to any expected type. Function-typed `noreturn` returns flow into if/else arms cleanly.

### ✓ Optional types

`Type.is_optional` flag added. `?T` and `?^T` parse to optional types. `nil → ?T` and `T → ?T` are assignable; `?T → T` is a type error (must unwrap). Display shows `?T` correctly. Equality and assignability respect the flag. STILL TODO: the `if (opt) |x|` unwrap form should bind `x` as the non-optional inner type — today the unwrap is loose. `sema_unwrap_optional()` exists for that future fix.

### Type and Decl are the only HIR

There's no separate IR layer. Sema works directly on AST `Expr*` plus `Type*` facts. That's fine for now, but two near-term things will need a real HIR:

- Codegen. Lowering an `Expr*` directly to assembly without a typed-IR is painful; AST is too unstructured.
- Comptime function calls (the missing interpreter from the comptime section). An interpreter wants something more like a typed instruction stream than raw AST nodes.

When we do build the comptime interpreter, the right move is to introduce a small typed IR (`HirNode`?) at the same time and have both interpreter and codegen consume it. Trying to do codegen on AST + facts will work for a while and then suddenly stop scaling.

### No diagnostic-recovery story for sema

Sema bails the moment `has_errors` flips. That's fine for v1 ergonomics, but it means the checker stops mid-walk on the first failure. Users of any non-trivial Ore program will see one error at a time. Foundation fix: keep walking past errors, mark affected expressions as `error_type`, suppress cascades. ~50 LOC scattered. Low priority but flagged.

### Module exports / pub default

The `pub` keyword is threaded through; the default-export-everything semantics is still in effect until we flip the one line in `collect_decl`. After the flip, top-level constructs without `pub` will be private to the module — matches Zig/Rust behavior.

What's actually next, ranked

1. ✓ TYPE_NORETURN — done.
2. ✓ Optional types (Type.is_optional) — done. If-let unwrap binding still loose.
3. ✓ Pointer-deref-field-access checker fix — done.
4. Comptime interpreter — the unlock for everything (build.ore, real generics, conditional comptime branches). Probably a 500–800 LOC comptime_eval.c that walks AST with a ComptimeEnv and runs functions. Pair with HIR introduction so it's a one-time ergonomic spend.
5. Effect-row unification — turns the open-row machinery from a placeholder into a working solver. ~200 LOC.
6. Flip `pub` to "private by default" — one-line behavior change in collect_decl. Then add a few `pub` annotations to the import test fixtures.
7. Codegen scaffolding — basic IR, lowering for arithmetic/loads/stores/calls/branches, x86_64 or aarch64 emit. Big.

Items 1–3 are foundation-level and now done. Items 4 and 7 are big features. Item 5 is medium. Item 6 is trivial once we decide.

Direct answer to "where are we"

Structural skeleton is in place for everything; algorithmic depth is at typechecker level. The query system, evidence vectors, instantiation cache, target abstraction, optional-types, noreturn, ParamKind — these are all the right shape for a 1.0 compiler. What's underdeveloped is the content of the analyses (no comptime interpreter, subset-only effect solver, no codegen).

The two foundation gaps that remain non-trivial are: the eventual HIR (foundational for both comptime interpreter and codegen) and effect-row unification. Everything else from the original gap list is closed or has a clear small follow-up (#2 if-let unwrap binding, #4 diag recovery, #5 pub default flip).
