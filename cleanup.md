OK, full audit. I'll be brutally honest — there are real gaps and several places where we cut corners that will bite later. Organized by priority.

🔴 Critical (touch before E.4)
1. Zero incremental tests
The entire query/invalidation cascade — slots, fingerprints, deps, the layer-7.5 walker — has never been exercised by re-parsing a module mid-session. All 20 fixtures run cold-start every time. We don't know if:

Editing a fn body invalidates only its body queries (the headline correctness property)
Editing one struct's fields cascades to dependents but not unrelated decls
The structural fingerprints actually do early-cutoff
Stale Expr* pointers in DefInfo.origin cause crashes after re-parse
This is the single biggest gap. Production compilers have hundreds of incremental tests. We have zero. Adding even five — "edit X, expect Y to re-run, expect Z not to re-run" — would surface real bugs immediately. Every other architectural concern is theoretical until we test invalidation.

2. DefInfo.origin is still cached Expr*
We added def_origin(s, def) and route through node_to_expr at population, but 27 read sites still read di->origin directly. After AST re-parse, those reads return stale pointers. We deferred the migration as "follow-up." That follow-up is load-bearing for incremental correctness.

3. is_comptime_evaluable is not a query
It's a direct recursive walker called once per bind_Const. For a const-bind chain like:


A :: 100
B :: A * 2
C :: B + A
D :: C - 1
D walks C and A, C walks B and A, etc. — quadratic in chain length. Worse: invalidation can't track the dep, so editing A doesn't trigger D's comptime-recheck.

Should be query_is_comptime(expr_id) with its own slot, fingerprint over (kind, all subexpression results), dep recorded on each child query. Same Salsa shape as everything else.

4. query_const_eval is severely incomplete
Today only handles Lit, Bin, Unary. Missing:

Ident resolving to another const-bind — BUF_SIZE :: MAX * 2 doesn't fold even when MAX is a literal
Builtin — @sizeOf(i32) types as comptime_int but its const value is CONST_NONE, so let x: u8 = @sizeOf(i32) passes the range check structurally without verifying
Field on comptime struct literals — origin.x doesn't fold to 0
If/Switch over comptime conditions
Block tail
Future: comptime fn calls
This means our coerce range-checking has silent holes — anything more complex than a literal expression skips verification.

🟠 Type system gaps (will block real code)
5. Coerce has no variance rules

// Current state of coerce:
if (from == to) return true;
if (from comptime_int) range-check via fits_in
if (from comptime_float) range-check
otherwise → error
Missing for any non-trivial program:

^T → ^const T (mutable ptr to const ptr — should always be allowed)
[]T → []const T (slice variance)
^[N]T → []T (array-pointer to slice — Zig auto-coerces; common pattern)
^[N]T → [^]T (decay to many-pointer)
nil → ^T / ?T (when those land)
noreturn → anything ✓ (we have this)
Without these, every fn that takes []const u8 (read-only string) will reject mutable slices. Real code becomes verbose with explicit casts.

6. ?T (optional type) doesn't exist
unary_Optional is in the AST. resolve_type_expr doesn't handle it. So fn foo() -> ?i32 errors at type position. This is a base-language feature in our orbit (Zig has it, Koka has Maybe).

7. String type is opaque, not []const u8
s->string_type = PRIM(TY_STRING, "string") — comments say "alias of []const u8 once slice semantics solidify." Slices have stabilized. We should reify this. Otherwise strings can't interop with slice operations like .len, indexing, slicing.

8. Function types in type position don't work

do_thrice :: fn(action: fn() -> i32) -> i32   // ERRORS
The error from earlier was "type expressions of this shape are not yet supported." resolve_type_expr doesn't handle expr_Lambda shapes in type position. Higher-order functions need this.

9. TY_PTR vs TY_MANY_PTR — pointer arith on [^]T
We split the kinds correctly, but no operation actually distinguishes pointer arithmetic. p + n where p: [^]T should produce another [^]T advanced by n*sizeof(T). Today bin_arith_result would error because pointers aren't numeric. Real arithmetic on many-pointers needs to be added.

🟡 Bidirectional check is partial
10. check_expr doesn't propagate through if / switch branches

let x: u8 = if (cond) 1 else 200    // each branch synth'd as comptime_int,
                                    // joined, THEN coerced to u8
That works because comptime_int → u8 with const value 1 / 200 each fits. But:


let x: u8 = if (cond) some_i32 else 200    // error, types don't match
If both branches were checked against u8, the i32 would error coercion clearly. Today we synth → join (strict equality) → fail with "branches don't match" which is misleading.

Same for switch arms — strict equality join, no bidirectional propagation.

11. Bidirectional for struct-literal field positions
When checking Point { .x = some_expr }, the field's expected type (the struct's field type) flows into check_expr for the value. ✓ We have this.

But for nested struct literals: Outer { .inner = .{ .x = 0, .y = 0 } } — the inner anonymous .{...} should resolve via the outer field's type. Does it? We do call check_expr(field.value, sig->fields[idx].type), which IS the bidirectional path. Should work, but worth a fixture.

🟡 Resolution / scope concerns
12. expr_DestructureBind not typed
Parser emits these for (a, b) := pair. Sema falls through to error. Used by tuple/struct destructuring. Common pattern.

13. Path resolution (query_resolve_path) — built but not exercised
Color.Red parses as expr_Field and goes through type_of_field. The query_resolve_path machinery exists but no actual code path uses it. Either remove (unused infra is rot) or wire something through it (e.g., module.fn access when @import lands).

14. Field DefIds allocated but never queried
query_struct_signature allocates DECL_FIELD defs eagerly. These DefIds appear in child_scope. No code ever calls query_type_of_def(field_def) because expr_Field goes through the signature directly. So those DefIds are dead infrastructure — RAM cost, no benefit. Either:

Use them (route expr_Field through path resolution → field DefId → type query)
Don't allocate them (lazy until needed)
The eager allocation made sense in the rust-analyzer-faithful version but doesn't pay off without a consumer.

🔵 Salsa-faithful concerns (Tier 3 from earlier)
15. No verified_at / changed_at split
Single fingerprint per slot. Any input change cascades fully. Salsa's optimization: a query can re-run and produce the same result (changed-rev unchanged), letting downstream skip via early cutoff at the slot level (not just fingerprint comparison). We collapse this. Real perf cost in incremental scenarios.

deferred 16. No untracked-read detection
Salsa's invariant: "every read inside a query is from another query's output." We don't enforce this at all. A query could accidentally read s->some_field directly (not through a query), miss recording the dep, and produce stale results after invalidation. Easy bug to write, hard to find.

Mitigation: a debug-only "trace mode" that logs every Sema field access and flags non-query reads. ~80 LoC. Not done.

17. Cycle recovery is just rejection
SEMA_QUERY_GUARD's on_cycle returns a sentinel value (usually error). Salsa supports fixpoint cycle recovery (CycleRecoveryStrategy::Fixpoint) — needed for type inference of self-referential types. Today's TY_STRUCT identity-only construction sidesteps this for nominal types, but type inference (E.5+ generics) will need it.

🟢 Effects-specific work needed (E.4)
18. lambda.effect AST exists but unused by sema
fn(...) <Exn> -> .... Parser produces lambda.effect. No type or check. Need:

TY_EFFECT or effect-row representation in the type system
Effect annotations on FnSignature
Effect propagation: a fn that calls an <Exn> fn must itself be <Exn> (or wrap with handler)
Handler scope tracking: with introduces evidence
19. Pattern matching limited to unit variants
Real PM needs: tuple/struct destructuring, range patterns, guards (if), capture binders (Some(x) => ...), nested patterns. The decision-tree compiler. ~1500 LoC of real work in HIR.

20. HIR not started for new typechecker
Legacy checker recorded HIR alongside typing. New (E.1+) typechecker doesn't. The CheckedBody.expr_hir HashMap exists; nothing populates it. For E.4's effect lowering and codegen, HIR must come back online.

⚪ Code-quality / hygiene
21. Code duplication across product-literal parsing
.{...}, Point{...} (new), and [N]T{...} all parse the same field-list shape. Three copies. Extract a helper.

deferred 22. Inconsistent error message phrasing
"expected X, got Y", "X requires Y", "cannot X to Y" — multiple styles. Diagnostic codes (E0100, etc.) referenced in comments but never wired. Future-you (and LSP integration) will want diagnostic codes.

23. Sema.name_* pre-interned names dead
name_sizeOf, name_TypeOf, name_intCast, etc. on Sema are declared but never initialized. Builtin dispatch uses string compares via pool_get instead. Either kill the fields or wire them up for hot-path comparison.

need back later 24. Several Sema fields declared but unused
anytype_type, effect_row_type, scope_token_type, effect_type — all on Sema, never assigned. Cleanup or wire.

My recommendation, prioritized for "100% correct before E.4":
Block E.4 on:

Add 5+ incremental tests (#1) — confirm invalidation works, surface latent re-parse bugs
Migrate DefInfo.origin reads to def_origin (#2) — completes the AST-staleness fix
query_const_eval for Ident → const-bind + Builtin (#4) — closes a silent correctness hole in coerce range-checking
Make is_comptime_evaluable a query (#3) — small change, big architectural consistency
Coerce variance rules for ^T → ^const T and friends (#5) — without this, real []const u8 use cases bottle-neck
Could defer to a separate cleanup PR but worth doing:
6. ?T (#6), string as []const u8 (#7), fn types in type position (#8) — base-language gaps that any non-trivial program will hit
7. Bidirectional through if/switch (#10) — affects diagnostic quality
8. Decide on field DefId allocation policy (#14) — either use them or don't allocate

Defer with eyes open:

Salsa-faithful verified/changed split (#15)
Untracked-read detection (#16)
Cycle fixpoint recovery (#17)
HIR (#20) — comes naturally with E.4 lowering
The blockers in the first list are ~600 LoC across ~6 files. They're mechanical work, not design decisions. After that, base language is genuinely production-shaped and E.4 can build on solid ground.

Want to tackle the blocker list?