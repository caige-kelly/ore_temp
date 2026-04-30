(c) Architectural gaps before full comptime / Koka effects / Zig-style build
Full comptime (Zig-grade):

✅ Generic specialization with cache.
✅ Body re-walk with substituted env.
✅ Const eval for ints, floats, bools, strings, types.
❌ Aggregate ConstValue — ComptimeStruct, ComptimeArray. Needed for comptime t: type = .{ .x = 1 } patterns.
❌ Comptime function calls — const_eval doesn't recurse into user functions. To run a function at comptime you need a tiny interpreter over the AST + the same ComptimeEnv. This is the biggest gap. Without it, comptime fn(x: type) type { ... } (Zig's metaprogramming workhorse) doesn't work.
❌ @typeName, @tagName, @returnType, @hasField — type-introspection builtins. Trivial individually, but the interpreter is the hard part; once that exists these are one-liners.
❌ Comptime if and switch at value level — branch elimination in the body re-walk based on env-known conditions. The dump shows comptime switch @target.os already in malloc.ore; today it walks both branches.
❌ Memoization correctness — Instantiation is keyed by (Decl, args) but args includes types. If a generic creates a new anonymous type per call (Zig does this), the cache key needs structural type equality. Today we have nominal equality only. Watch for this when you add real generics tests.
Koka-grade effects:

✅ Effect declarations, ops, <E> annotations, open rows.
✅ Evidence vectors at use sites.
✅ Sig-vs-body checking (with limitations above).
❌ Effect-row unification — open rows <X | e> should unify with concrete sets at call sites and propagate up. Today solve_effect_rows only checks subset. A proper solver maintains substitutions and reports unification failure.
❌ Higher-order effect polymorphism — fn(action: fn() <e> R) <e> R (mask/inject). Requires effect rows as first-class type variables. Annotations parse it, sema doesn't yet do row-variable substitution at instantiation.
❌ Multi-shot continuations — Koka's ctl ops can resume zero or many times. mp_prompt supports this, but our EvidenceKind doesn't distinguish single-shot from multi-shot, and the body re-walk doesn't track whether resume is called 0/1/N times.
❌ Scoped tokens (scoped effect<s>) escape checking — scope_token_id is on Decls, but no analysis checks that a value carrying a scope token doesn't escape its handler. This is the borrow-lite story your notes already flag.
❌ Handler resolution — when codegen emits mp_yield, which evidence slot? Today the evidence vector is built but no analysis maps "this perform-call site → that frame." Add sema_evidence_for_perform(s, call_expr) → EvidenceFrame* that does the matching.
Zig-style build system:

❌ build.ore as a comptime program — needs full comptime function calls (above).
❌ Target abstraction read from build, not host — we have TargetInfo; today target_default_host() is the only constructor. Need TargetInfo target_from_comptime(...) that runs build.ore and reads the configured target. Hookpoint exists; constructor is missing.
❌ Build options as comptime values — @build.enable_stack_traces in malloc.ore is a thing today. const_eval needs an @build table passed in via the comptime env, populated from the build file.
❌ Module graph beyond imports — Zig builds run multiple compile units linked together. Today Sema is single-module-rooted (despite walking all modules in the compiler). The module_loader does dependency resolution but there's no real notion of "this module is a separate object/exe artifact." That's a build-system-level concept that needs a BuildGraph above Compiler.
❌ Caching of pass results across runs — Salsa-grade incremental. Way out. But the queries are already keyed correctly for it; what's missing is durable IDs (we use Decl*/Type* pointers, which won't survive a re-parse).
What I'd actually clean up next, in priority
#11 (with-discharges-everything) — wrong-by-design, easy to fix with the evidence resolver we already have. 15 lines.
#1 / #2 (dead effect_sigs, fact API leakage) — clarify what's owned where. small refactor.
#6 (lying EvidenceKind) — either delete TAIL_RESUMPTIVE or implement it. Probably delete until inference exists.
#3 / #8 (Decl-as-dumping-ground, is_handler_impl placement) — bigger but pays back every future feature. Move sema cache off Decl.
Comptime function calls (interpreter) — this is the single biggest unlock for everything else (build.ore, real malloc.ore, Zig metaprogramming). Probably a 500-LOC comptime_eval.c that's a tree-walking interpreter over the AST, sharing types with const_eval.c. Without this, "comptime" is a misnomer for what we have.
Should evidence vectors be on Compiler instead of Sema?
No — they're per-checking-pass state. A Compiler runs multiple passes over multiple modules; evidence is computed during sema for one specific module's pass. The right home is exactly where it is: Sema.current_evidence (mutable, walks with the AST) + CheckedBody.entry_evidence / call_evidence (immutable snapshots, owned by sema results).

What should move up to Compiler is the TargetInfo (today on Sema, but it's process-wide), StringPool (already there), arena and pass_arena (already there). These are correct.

Verdict
The compiler is well-positioned for full comptime, Koka effects, and Zig builds in the sense that the right abstractions exist. Queries, CheckedBodies, Instantiations, EvidenceVectors, ConstValue, ComptimeEnv — these are the load-bearing pieces and they're shaped right.

It's not yet positioned in the sense that the actual analyses behind those abstractions are partial: const_eval doesn't run user functions, the effect solver is subset-only not unification, evidence inference is structural-only not tail-resumptive, and there's no build graph above the compiler. None of these require redesigning what's there — they require filling in the bodies of analyses we've already named.

The single biggest leverage for everything is (c #2): comptime function calls. That unlocks build.ore, real type-introspection, conditional comptime branches, and meaningful generics. Everything else is incremental.

Want me to do the four small cleanups (#11, #1, #2, #6) first, then start on the comptime interpreter?