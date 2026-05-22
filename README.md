# Ore

## Introduction

Ore is a systems programming language designed for ultimate modularity. By combining Algebraic Effects (Koka-style) with Compile-time Metaprogramming, Ore allows you to define not just how your code runs, but how it interacts with the system at a fundamental level.

## Key Features

Algebraic Effects: Replace brittle error handling and global state with scoped, type-safe handlers.

Comptime Power: A full execution engine at compile time for DSLs and code generation.

Zero-Dependency Core: The compiler provides the primitives; the community provides the structures.

## Code at a glance

```rust
allocator :: @import("allocator.ore")
a         :: allocator.Allocator

main :: fn(void) i32
    comptime if (@build.mode == .debug)
        with @build.handlers.exn
        with @build.handlers.allocator

    r1 := alloc(u8, 1024)
    defer free(r1)

    0
```

## Build System Example

Ore has utilizing a build system written in Ore.

```rust
b :: @import("./build.ore")

build :: pub fn(void) void
    exe := b.add_executable(.{
        .name = "allocator_test", 
        .root = b.root(.{
            .source_file = b.path("main.ore"),
            .target = .APPLESILICON,
        })
        .handlers = b.default_handlers(.{
            .{ .name = "allocator", .fn = @import("allocator.ore").debug_allocator }
            .{ .name = "exn", .fn = @import("exn.ore").default_exn }
        })
    })

    builder.install_artifact(exe)
```

Target Architecture — synthesized from salsa-rs + zig + rust-analyzer
What each one actually does
salsa-rs: Single flat Vec<Box<dyn Ingredient>>. Every "kind of stored thing" (input field, tracked fn, accumulator, interned struct) is its own ingredient. Per-instance memos live attached to the struct's MemoTable, not in a side-table indexed by ID. Inputs are TRACKED — every db.field(sid) call inside a query records a dep via thread-local ZalsaLocal. NO reverse indexes in core — user code maintains them. Diagnostics surface via accumulators (type-erased per-ingredient Vecs on the active-query frame). No GC; old memos defer-dropped at new revision.

zig: Hierarchy is Compilation → Zcu → File → Nav. One giant InternPool for everything (types, values, AND nav/decls — tagged Item discriminator). Nav is AoS — name, fqn, namespace, analysis-state-enum all in one struct. Variable-size payloads (struct fields, fn params, enum variants) live in shared extra[] u32 pool. Diagnostics centralized in Zcu.failed_analysis: HashMap<AnalUnit, *ErrorMsg>. Bidirectional dep graph IN the InternPool (nav_val_deps, src_hash_deps — doubly-linked list nodes). NO node → enclosing decl reverse map.

rust-analyzer: Three layers — Syntax (rowan) → AST (typed wrappers) → HIR. Plus an ItemTree layer between AST and HIR specifically for incremental name resolution (body edits don't invalidate item structure). Per-kind salsa-interned IDs (FunctionId, StructId, EnumId are distinct types). Per-body data in a Body { exprs: Arena<Expr>, pats: Arena<Pat>, bindings: Arena<Binding>, labels: Arena<Label>, ... }. ExprScopes is a query (matches what we built). Diagnostics returned IN query results ((Result, Vec<Diag>)) — NOT the accumulator pattern. Find-refs/goto-def computed on-demand via text-search + name resolution.

The biggest divergence from us
None of the three uses column-per-field SoA the way we do for db.defs.

Salsa: per-instance memos (AoS-ish, ingredient-organized)
Zig: Nav is one struct with all state (AoS)
rust-analyzer: per-kind data structs (FunctionData, StructData), each AoS
We have parallel columns: defs.names[i], defs.ast_ids[i], defs.parent_modules[i], defs.types[i], defs.fn_sigs[i], defs.values[i], defs.effect_sigs[i], slots_type[i], slots_signature[i], slots_infer[i], slots_body_scopes[i], slots_const_eval[i], body_scopes[i]. 12 parallel columns indexed by DefId.idx. For any operation that touches one def's full state (hover, goto-def, type render), we cache-miss across 12 separate column allocations.

This is a real architectural divergence. Salsa-rust and Zig converged independently on "per-instance state lives together"; we went the other way.

Target architecture
1. Storage organization — per-kind side tables
Replace wide-SoA-by-DefId with per-kind side tables. The model is closer to Zig's Nav + rust-analyzer's FunctionData/StructData:


db.defs.kinds  : Vec<DefKind>       // DefId.idx → kind discriminator
db.defs.names  : Vec<StrId>         // shared by ALL kinds (always needed)
db.defs.ast_ids: Vec<AstId>         // shared by ALL kinds
db.defs.parent_modules : Vec<ModuleId>  // shared by ALL kinds

db.fns         : Vec<FnRecord>      // one entry per fn def
db.structs     : Vec<StructRecord>  // one entry per struct def
db.enums       : Vec<EnumRecord>
db.consts      : Vec<ConstRecord>
db.effects     : Vec<EffectRecord>

// Routing: DefId → kind-specific row
db.defs.kind_row : Vec<uint32_t>    // idx into the appropriate kind table
Each FnRecord is AoS: signature, body_scopes pointer, infer_slot, signature_slot, all together. Cache-coherent for any operation touching a fn.

This isn't "less SoA" — it's MORE SoA done at the right granularity. Today we have 12 parallel columns where 8 are wasted for non-fn defs. Per-kind tables make every column dense.

2. Diagnostics — centralized table, not per-slot
Replace per-slot diag accumulators with a single keyed table. Zig's pattern:


HashMap failed_analysis;   // AnalUnit → ErrorMsg* (where AnalUnit = (QueryKind, key))
HashMap failed_codegen;    // FnId → ErrorMsg*
HashMap failed_files;      // FileId → parse errors
Wins:

db_collect_diags_for_file walks ONLY the hashmaps, not every slot. O(error_count) not O(slots).
Diags don't get orphaned when a slot goes EMPTY without recompute — they're keyed by analysis unit, cleared explicitly on invalidation.
Single source of truth: "where do errors live?" → these tables.
The salsa accumulator pattern is the alternative; Zig's per-AnalUnit map is simpler and what we actually want for a single-threaded compiler.

3. Inputs — keep our current model, document it
Salsa's tracked input getters (every db.source_text(sid) call records a dep) would give us per-field invalidation precision. We don't need it.

Document the rule: our inputs are read EXCLUSIVELY by designated "input adapter" queries (QUERY_FILE_AST reads source text; nobody else does). Those queries call db_query_note_input_durability to declare the coarse dep. All other code reads inputs via db_get_* accessors WITHOUT dep recording, and trusts that they're inside a query body whose adapter dep covers them.

This is a deliberate simplification, not an oversight. Salsa's per-field tracking buys precision we don't need at our scale.

4. Per-instance memo storage — keep pointers, document
Salsa stores memos attached to struct instances via MemoTable (per-instance pointer). Zig stores extras in a shared pool with indices.

Our slot->diags, slot->diag_arena, slot->deps are per-slot pointers. Keep them, but document: this matches salsa's per-instance model. The DOD-purist "flat global pool with GC" alternative is more code without measurable benefit at our scale.

The one we SHOULD change: defs.body_scopes is Vec<BodyScopes*> (pointer indirection). Move BodyScopes inline into the FnRecord (per #1). Same for the inner Vecs (scope rows, binds, node_to_scope) — they become per-fn ranges in shared db.body_scope_rows, db.body_scope_binds, db.node_to_scope pools.

5. Per-file data — flat pools indexed by file
files.line_starts, files.trivia_tokens, files.trivia_offsets are Vec<Vec<...>> today. Flatten:


db.line_starts_pool : Vec<uint32_t>
db.line_starts_off  : Vec<uint32_t>  // per-file (start, count) range
db.trivia_pool      : Vec<TriviaSpan>
db.trivia_off       : Vec<uint32_t>
db.trivia_offsets   : Vec<uint32_t>
db.trivia_offsets_off : Vec<uint32_t>
The per-file arena resets on reparse, so we know the regeneration boundary. Use the modules.file_pool/file_offsets idiom we already have.

6. Reverse indexes — only the ones we use
We added AstNodeId → DefId via ModuleNodeData.defs. Keep it.

DON'T add eager reverse indexes for find-refs, callgraph, etc. Salsa, Zig, and rust-analyzer all compute these on-demand. Wait until profiling shows a hot path.

7. Layer model — stay at two, plan for three
We have Source → AST → sema. Rust-analyzer has 5+. Zig has 4 (AST → ZIR → AIR → output).

Stay at two for now. When we hit incremental pain (body edits invalidating item structure), add an ItemTree-equivalent — a per-file simplified item table that survives body edits. That's the single biggest win rust-analyzer gets from ItemTree.

For now, our AST already serves this — module_exports walks top-level decls without re-doing bodies. Good enough.

8. Memory reclamation — no GC, defer at recompute
Salsa: old memos defer-dropped at next revision boundary.
Zig: arenas reset on incremental rebuild.
Our current: per-slot arena reset on compute path (already correct).

Stay with what we have. Don't add a GC.

9. Boundary — convention + headers, no enforcement
C can't enforce. We already have setters/getters/query folders + db.h boundary sections. That's the ceiling unless we want a build-time linter.

Document and stop worrying about it.

Migration delta — what changes from current to target
Ranked by impact / effort ratio.

#	Change	Effort	Wins
1	Centralized diag tables keyed by (QueryKind, key) HashMap, replacing per-slot diag arena	Medium	LSP-scale diag collection becomes O(error_count). Stale-diag invariant is now mechanical (clear on invalidate).
2	Per-kind side tables (db.fns, db.structs, db.enums, etc.) replacing wide SoA in db.defs	Large	Dense storage, cache-coherent per-def access, drops 5+ wasted columns per non-fn def.
3	Inline BodyScopes into FnRecord, body_scope data into 3 shared pools	Medium	Kills 4N malloc allocations (N = fn count). Cache-friendly scope walks.
4	Flat-pool line_starts, trivia_tokens, trivia_offsets with per-file ranges	Small	Kills Vec<Vec> pattern. ~3N malloc savings per reparse.
5	Document input adapter pattern: list which queries read which inputs; rest read via getters under that umbrella	Small	Makes the dep-tracking-precision tradeoff explicit. No code change.
6	Plan room for ItemTree-equivalent layer when incremental name resolution becomes a bottleneck	None now	Architectural awareness; no current change.
Stuff we should NOT do based on the research:

Don't adopt tracked input getters (over-engineered for single-thread)
Don't add a GC for memos (salsa doesn't have one, neither does Zig)
Don't pre-build reverse indexes for find-refs / callgraph
Don't split the db trait surface — it's a Rust-orphan-rule workaround
Don't adopt salsa's accumulator pattern — Zig's per-AnalUnit map is cleaner for us
Suggested order of operations
I'd write this as a 2-3 week sequence, NOT a single PR:

First: #1 (centralized diags) — biggest LSP scale win, low risk, no public API breaks
Then: #4 (flat-pool per-file data) — small, mechanical, ASan-verifiable
Then: #3 (BodyScopes flattening) — moderate, but well-contained in body_scopes.c
Finally: #2 (per-kind side tables) — biggest refactor, touches every sema query. Do this last when the foundations are settled.
#5 and #6 are documentation/awareness items.

Update Todos

Target-architecture synthesis from salsa/zig/rust-analyzer

Migration delta plan
That's the synthesis. Reading the three reports together, the strongest signal was the AoS vs SoA-by-field divergence — we're an outlier doing column-per-field, and both Zig and salsa converged on "per-instance state lives together." That's the deepest architectural change to plan for.

The smaller items (centralized diags, flat per-file pools, body_scopes flattening) are all defensible incremental moves. The big one (per-kind side tables) is a real refactor that deserves its own sequencing.

Want me to write this up as a markdown doc in the repo (e.g. docs/architecture.md) so it's checkable into git and can be the reference next time we drift?