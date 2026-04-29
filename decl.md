## The compile error

`decls.c` doesn't include `compiler.h`, so `struct Compiler` is opaque there. Add at the top:

```c
#include "../compiler/compiler.h"
```

That's it. `sema.c` already does this; `decls.c` just hadn't needed it before.

## Production-grade spectrum

What you have now is the simplest correct shape: single linear pre-pass + opportunistic cache updates during expression walk. Real compilers add more layers as the language grows. Here's the progression, from "small but proper" to "rustc-grade":

### Level 1: Two-phase signature resolution

The single biggest upgrade. Splits "build the empty shell" from "fill in the body":

```text
Phase 1a (skeleton):  for every top-level Decl, allocate an empty Type with
                      just `kind` and `decl` set. Don't infer params, fields,
                      returns, or bodies. Just stake out the slots.

Phase 1b (signature): walk all Decls again. Fill in struct fields, function
                      params/return, effect operations. Every reference to
                      another Decl finds the skeleton from phase 1a, so
                      mutual recursion works.

Phase 2  (bodies):    type-check function bodies. By now every Type that
                      could be referenced has a complete signature.
```

Why this matters: handles `Foo :: struct  next : ?^Bar` ↔ `Bar :: struct  prev : ?^Foo` cleanly. Your current single-pass code falls over on cyclic types because the second-defined type sees the first as `unknown`.

It also lets you naturally handle `bar :: foo + 1` followed by `foo :: 42` — phase 1 doesn't care about source order.

The cost: each `Type*` lifetime now spans phases — you fill in fields after allocation rather than at allocation time. Means `sema_type_new` returns a partially-initialized Type for nominal kinds. Add an `is_complete` flag if it helps debugging.

This is what Go, OCaml, Swift, and Rust all do at the package/module level. **This is the next upgrade you should plan for.**

### Level 2: SCC-aware processing for inference

When you can't figure out a type without knowing its dependencies (e.g., recursive type-class inference, mutually-recursive functions inferred without annotations), you need to:

1. Build a dependency graph among Decls.
2. Run Tarjan's SCC algorithm.
3. Process SCCs in reverse topological order. Decls within one SCC are inferred together.

This is how ML, Haskell, and Rust do let-rec/method inference. Without it, mutually-recursive un-annotated functions are uninferrable.

You can skip this for a long time — Ore's bidirectional checker requires annotations on top-level functions (just like Zig and Rust), so SCC inference isn't strictly needed. Comes up if you ever want full inference for top-level value bindings.

### Level 3: Query-based architecture

What rustc and Roslyn do. Replace eager pre-passes with a memoized query system:

```c
struct Type* type_of(struct Sema* s, struct Decl* d) {
    if (d->cached_type) return d->cached_type;
    if (d->in_progress)  { /* cycle - emit error, return error_type */ }
    d->in_progress = true;
    struct Type* t = derive_type(s, d);
    d->cached_type = t;
    d->in_progress = false;
    return t;
}
```

Benefits:
- Self-organizing — you don't have to figure out the right phase order; the query graph is implicit.
- Cycle detection comes for free (the `in_progress` flag).
- Easy to add more queries (`size_of`, `align_of`, `effect_sig_of`, `instantiation_of`) — they all follow the same pattern.
- Maps directly onto incremental compilation later.

The cost: every query needs the cycle-guarding boilerplate, and "which queries depend on which" becomes implicit rather than explicit. For a small language, the explicit phases are usually clearer; you flip to queries once the dependency graph gets too tangled to maintain manually.

Recommend deferring this until you actually have ≥5 distinct things to compute per Decl (type, size, align, effect_sig, instantiation_set, etc.). Below that, explicit phases are fine.

### Level 4: Generic instantiation table

For Zig-style comptime / generic specialization. Each call to a generic function with concrete comptime args is a separate Type. Keyed by `(generic_decl, comptime_arg_tuple)`:

```c
HashMap<(Decl*, ComptimeValue[]), Type*>
```

Each lookup either hits an existing instantiation or triggers a fresh one (which kicks off type-of/size-of/etc. for the new instance). The cache key is what makes specialization tractable — without it you'd re-walk and re-build for every call site.

You'll need this in Phase 6 of the plan doc when `comptime t: type` parameters land. Until then, irrelevant.

### Level 5: Incremental cache + dependency tracking

For IDEs / fast rebuilds. Tracks "if Decl X's type changes, which other Decls need re-typing?" Implies:
- Persistent IDs (not pointers — pointers don't survive AST regeneration).
- Each query records its dependencies.
- Edits invalidate the smallest possible set.

This is Salsa (rustc), TypeScript project references, Roslyn workspaces. **Not relevant** until you're past 1.0 and building tooling.

### Level 6: Proper symbol IDs, persistent across edits

Same idea as level 5 but for the symbol table itself. Every Decl gets a stable ID derived from its declaration path (`module/scope/name`), not from its memory location. Lets you serialize the type cache and reload across compiler runs.

**Definitely not relevant** for a long time.

## Where I'd actually aim

For your roadmap as written:

1. **Now:** ship the minimal Decl-cache fix as drafted, fix the loop_control test.
2. **Soon (before Phase 3 comptime):** upgrade to Level 1 (two-phase signature resolution). Cyclic types and forward references stop being a hidden hazard.
3. **When you start specialization (Phase 6):** add Level 4 (instantiation table). The query-style architecture (Level 3) becomes attractive at the same time, because by then you'll have ≥5 things to compute per Decl.
4. **Levels 2, 5, 6:** ignore until they actively block something.

Don't build query-style or SCC inference now. The minimal cache + a future two-phase upgrade will carry you through Phase 5 of the plan doc. The Level 1 upgrade is maybe a half-day of work; it's the only one you should mentally schedule for soon-ish.

The other things to keep in mind even at the minimal level:

- **Cycle detection.** Today if `Foo :: Foo` (self-referential bind), `sema_infer_expr` will infinite-recurse. An `in_progress` flag on each Decl, checked at the top of `sema_type_from_decl`, gives you a cheap "cycle in declaration" diagnostic. Worth adding.
- **Diagnostic for "type couldn't be determined"**: if `cached_type` ends up `unknown_type` for a `SEM_VALUE` Decl that should have one, emit a single error rather than letting `unknown` poison every downstream check (which is exactly the loop_control symptom). This is the "one-shot error" pattern: surface the root cause once, suppress cascading.

Both fit into the minimal version with no architectural change.