# Phase 0 — Test gate audit OUTPUT

## Contract: load-bearing behaviors the rewrite must satisfy

These are confirmed locked-in by the existing test gate. The new code must preserve all of them.

### Salsa correctness (decl_incremental, file_incremental, source_edit, durability)
- C1. Edit A's body → only A's 4 sema slots recompute; B's slots frozen at prior revision
- C2. Structural hash is position-independent (B's hash unchanged after A's length change)
- C3. Trivia (whitespace) does not trigger recomputation
- C4. Non-kind extras in structural hash (param names included; renames trigger recompute)
- C5. Byte-identical edit is a fast-path no-op; version not bumped, no reparse
- C6. Real edit bumps version + triggers reparse
- C7. Per-file early-cutoff: unedited file's QUERY_FILE_AST is verified-unchanged and skipped
- C8. Durability fast-path: when MIN over inputs is HIGH and current rev > HIGH-tier last_changed, skip dep walk entirely (verified_rev stays old)
- C9. Storage bounded across edit sequences (no malloc leak in 64-edit loop)

### Type identity / cycles (cycle_struct, sema_type)
- C10. Self-referential struct via `^T` materializes via wip-publish, not infinite recursion
- C11. Mutually-referential structs A↔B materialize; both types non-IP_NONE; field types reference each other's IpIndex
- C12. Named type equality tied to Decl identity, not just name
- C13. Pointer type equality + nil coercion + unknown polymorphism

### Scope / lookup (scope_shadowing)
- C14. Nested `y := 2` in block has distinct scope_id from outer `y := x` (separation only — lookup correctness NOT proven, see Gap G6 below)

### Node router / hover (node_type_router, lsp_test)
- C15. KIND_FUNCTION sig: param `x: i32` → IP_I32_TYPE
- C16. KIND_FUNCTION body: ref `x` → IP_I32_TYPE
- C17. KIND_CONSTANT fallback: const decl name → its type
- C18. KIND_STRUCT: struct field `p: i32` → IP_I32_TYPE
- (UNION, ENUM, VARIABLE, EFFECT, HANDLER NOT covered — see Gap G7)

### Hash-cons / parser (reparse_churn, parser_green)
- C19. Whitespace-only edit produces same GreenNode* for all top-level decls (100 reparses)
- C20. Edit one decl → that decl's GreenNode changes, siblings' GreenNodes unchanged
- C21. Revert to original content re-interns to original GreenNode pointer

### Workspace / imports (import_resolution)
- C22. workspace_resolve_import("./b.ore") from file in nsA resolves to nsB
- C23. db_query_namespace_type(nsB) materializes IPK_NAMESPACE_TYPE with pub exports
- C24. Cross-module type binding (`g := b.greet`) types correctly, no errors

### Diagnostics (diag_render, lsp_test)
- C25. db_format_diag substitutes StrId args via %S
- C26. db_resolve_span converts TinySpan to 1-indexed (line, col_start, col_end) + path
- C27. Diag anchor follows AST node, not byte offset (sticky squiggle on text shift)
- C28. Unused decl analysis: private unused → warning, pub → no warning, referenced → no warning

### LSP integration (lsp_test)
- C29. didOpen publishes diagnostics
- C30. didChange invalidates dependent files (drop pub Foo in A → B's `a.Foo` errors)
- C31. Hover on struct field, array size, primitive, fn decl, param all return correct types
- C32. Per-node type cache survives sibling edits (cached hover types stable)
- C33. Self-referential struct hover (`next : ?^Node` resolves)
- C34. Pool compaction under stress (60 didChange cycles) doesn't corrupt type ranges

### Determinism + composition (determinism_test, sema_const_eval)
- C35. Bytewise determinism across 41 parser fixtures (two runs produce identical stdout+stderr)
- C36. Const eval mechanics: blocks, loops, calls, struct/array literals, mutable bindings, control flow (NOTE: stubbed for sema integration)

## Gaps the rewrite must address (P0 — must-add tests before rewriting)

These are load-bearing behaviors not currently locked in. The rewrite is at risk of regressing them silently.

- **G1. Cancellation semantics** — cancel mid-query → slot cleanly resets → next call returns to COMPUTE
- **G2. Cycles beyond structs** — enum-of-fn-types, union-of-self-ref, anonymous tuple/array of self
- **G3. Push-stamp consistency** — after parse, per-decl slots match current revision (critical for the new push model)
- **G4. Hashmap dep dedup** — recording the same dep twice within one query body stores it exactly once
- **G5. Failure-then-retry** — query_fail → cached ERROR → next edit transitions back to COMPUTE, not stuck on ERROR
- **G6. Scope lookup correctness** — actually call `sema_body_scope_lookup` at the inner-block use site and assert it returns the INNER y, not the outer (existing scope_shadowing_test only checks scope-id separation)
- **G7. Node type router for ALL KIND_* branches** — KIND_UNION, KIND_ENUM, KIND_VARIABLE, KIND_EFFECT, KIND_HANDLER (existing test covers 3 of 8)
- **G8. Import cycle detection** — A imports B, B imports A → graceful error, not OOM (matches audit Tier-0 B2)
- **G9. Multi-level import cascade** — A imports B imports C, edit C, observe A re-resolves types correctly
- **G10. Diag slot lifecycle** — query re-runs → its old diags cleared before new ones emitted (not accumulated)

## Gaps to add (P1 — nice-to-have, can wait until validation phase)

- G11. Body_scope pool compaction explicit test (currently only proven by LSan post-mortem)
- G12. Hash-cons per-token stability (only per-node is tested today)
- G13. NodeCache eviction policy under sustained sustained load
- G14. Diag with secondary spans (related-info pattern)
- G15. Workspace eviction + re-open round-trip (same data observed)

## Existing weak assertions to harden DURING the rewrite (not before)

- W1. decl_incremental Edit 1 — verify *which* slots ran, not just compute_count deltas
- W2. source_edit edit loop — verify content actually changed across 64 edits (not just computed_rev advancing)
- W3. durability_test — assert HOT column population directly
- W4. node_type_router — add KIND_UNION/ENUM/VARIABLE/EFFECT/HANDLER probes (overlaps with G7)
- W5. reparse_churn — direct memory growth bound, not just LSan post-mortem
- W6. sema_type display string — semantic checks instead of exact-string match
- W7. lsp_test hover substring contains — structural assertions instead of substring contains

## Phase 0 action items

Before any rewrite code:
1. Write 10 new test files for G1–G10 (P0 gaps).
2. Verify each new test FAILS in expected ways against an instrumented stub of the current code (sanity check that the test actually exercises the behavior).
3. Run the full test gate green to lock in the baseline.

Estimate: ~3-4 days of test writing. The rewrite then has a true contract to validate against.

---

Starting fresh on src/db/query/ lets us design for:

ARCHITECTURAL PRINCIPLES (current engine state post-H1..H24 + A1..A4):

Per-entry queries from day 1 — no migration, no "wait, we still need top_level_index for compat". The top_level_indices per-file aggregating column is deleted; enumeration is by iterating per-name TOP_LEVEL_ENTRY slots.

Pure query model — each query's memoized result is owned by its slot, stored in a named result column. The slot's state + fingerprint are the validity check; the result column IS the data. No separate side-effect destinations; results never live in storage the slot doesn't own. Side-data (per-fn node-type tables, scope maps) is folded into per-query result structs (FnSignature, FnBody, VariableType, ConstantType, StructType) so a single column row holds the whole result.

Push-stamp primitive built in — the file_ast → decl_ast push pattern is a first-class engine concept, not a bolt-on. Push-stamp also propagates the pusher's durability tier to the stamped slot.

Push-stamped slot validity — a slot transitioned to DONE via db_query_stamp_direct is valid IFF cold->stamped_rev == current_revision. Dep walks do not apply to stamped slots; the authoritative producer is the pusher, and a slot not re-stamped at the current revision is stale by definition.

Engine-enforced contracts — internal vs external API separation enforced at compile time (ORE_ENGINE_PRIVATE guard on engine_internal.h AND result_columns.h). Dispatch exhaustiveness checked at compile time via X-macro over ORE_QUERY_KINDS.

Hashmap dep dedup from start — per-frame dep_index HashMap gives O(1) record + dedup.

Salsa-style verify short-circuit — every slot caches verified_rev (last revision proven valid). Verify path: (1) if verified_rev == eff, trivially valid; (2) durability fast-path: if dur_last_changed[slot.durability] <= verified_rev, skip the dep walk; (3) otherwise walk recorded deps. Matches salsa::shallow_verify_memo (salsa/src/function/maybe_changed_after.rs:235-266).

Cycle handling unified — re-entry into a RUNNING slot returns CYCLE without state mutation; DB_QUERY_GUARD's on_cycle arg gives the layer code a single semantic recovery point. No per-query manual cycle handling.

Cancellation never caches — db_query_succeed / _fail / _stamp_direct check db_check_cancel at entry. If cancellation is in flight, the slot transition is skipped (slot stays RUNNING with deps assigned). db_engine_sweep_running at request_end resets RUNNING slots to EMPTY AND frees their malloc-backed deps/dep_index buffers. The wrapper's result column may carry computed-from-canceled-sentinel garbage, but slot.state != DONE makes it invisible to consumers — next request recomputes from clean inputs.

Universal routing-key stamping — every slot's cold->routing_key holds the real query key (set on EMPTY→RUNNING transition AND on stamp_direct). HashMap-routed AND row-indexed kinds use the same field. Orphan reclamation reads it for db_diags_clear so diags emitted under the call-time key are cleared under the SAME key — no synthetic-row-index drift (which would leak diags for library files where fid.idx has the high bit set).

Pointer-stable slot + result storage — all slot columns (per-kind tables for fns/structs/unions/enums/effects/handlers/variables/constants; HashMap-routed slot columns for decl_ast/top_level_entry/def_identity/resolve_ref/resolve_path; files/namespaces slot columns) AND pointer-returning result columns are PagedVec-backed (Salsa-style Page<T>: fixed 1024 elements per page, packed (page<<10)|slot indexing). Pages, once allocated, never move — readers can hold pointers across subsequent pushes without invalidation. Eliminates the engine_verify re-locate-after-pull dance and makes result_columns.h's const FnSignature * / const FnBody * returns provably safe for the request's lifetime. PagedVec.count is _Atomic from day 1 so single-producer publish to multi-threaded readers is safe under memory_order_release/acquire (compiles to plain load/store on x86 single-threaded). Multi-producer push (CAS loop) is deferred until the parallel-query phase. Reference: salsa/src/table.rs:103-121 (PAGE_LEN_BITS=10) + lines 538-551 (packed ID encoding).

Heap-buffer ownership at reclamation — slot reclamation frees the slot's deps Vec and dep_index HashMap data buffers (malloc-backed; only the Vec/HashMap STRUCTS are arena-allocated). Per-kind reclaim hook (engine_compact.c::free_type_slot_result_heap) frees HashMaps embedded in result structs (FnSignature.node_types, FnBody.scope_map, VariableType/ConstantType/StructType node-types). No leak across orphan reclamation OR per-recompute slot reuse.

Compaction integrated with slot lifecycle — slot orphan reclamation AND shared-pool mark-and-copy both run in db_request_end → db_engine_compact. Threshold-gated per-pool so unchanged pools don't pay the cost. Pools compacted: body_scope_rows + body_scope_binds (driven by FnBody walk over DONE BODY_SCOPES slots), and scopes.decl_pool (driven by NamespaceScopes reachability + primitives_scope).

Production telemetry — query_stats[kind] counters always on (begin / cached_hit / compute / cycle / error / orphan_reclaimed). compact_stats tracks pool compaction frequency, bytes reclaimed, ns spent. Surfaced via --dump-query-stats and the profile-workload harness.

The result is the architecture you'd have if you'd known then what you know.

---

FUTURE COMMITMENTS (designed but not yet implemented):

Parallel queries — current engine is single-mutator. Page-based slot/result storage has ALREADY shipped (Phase A: PagedVec primitive + migration of all slot columns + pointer-returning result columns). The remaining parallel pieces land together when parallelism is a real requirement:

  Required pieces still pending:
  - Slot state field becomes atomic (atomic<QueryState>) so verify reads on one thread don't race with state transitions on another. (Memory-order overhead is wasted single-threaded; mandatory multi-threaded.)
  - QueryFrame stack moves from db.query_stack to thread-local (per-ctx). The db_query_ctx typedef in engine.h is the seam — it becomes a real struct {db *db; QueryFrameStack stack; CancellationToken cancel}. (Pure API refactor; no perf change.)
  - PagedVec gains a multi-producer push (CAS loop on count + race-free page allocation). Today's single-producer paged_push uses atomic_store_explicit(release); multi-producer needs CAS on the count field. Pages still never move.
  - SyncTable (per-kind Mutex<HashMap<key, ClaimState>>) coordinates inter-thread "this slot is being computed elsewhere" handoff; only contended at claim time, never on cache hits. (Dead weight single-threaded.)
  - DependencyGraph for cycle detection across threads (when thread A waits on a slot claimed by thread B, record the edge; detect inter-thread cycles). (Dead weight single-threaded.)

  Already shipped (Phase A foundation):
  - PagedVec<T> in src/support/data_structure/paged_vec.{h,c} — fixed 1024-element pages, _Atomic count, pointer-stable across pushes. paged_get is O(1) via page<<10|slot indexing.
  - All slot columns + pointer-returning result columns migrated from Vec to PagedVec. Result columns held as `const T *` returns are now provably stable for the request's lifetime.
  - Engine no longer needs re-locate-after-pull in verify; pointer obtained before sub-query stays valid after.

  References (read together as one design):
    salsa/src/table.rs (Page<T>) — adopted as our PagedVec.
    salsa/src/table/memo.rs (AtomicPtr<MemoEntry>) — for multi-thread memo updates.
    salsa/src/zalsa_local.rs (ZalsaLocal thread-local) — for per-ctx frame stacks.
    salsa/src/function/sync.rs (SyncTable) — claim coordination.
    salsa/src/runtime.rs (DependencyGraph::block_on) — inter-thread cycle detection.

  Sequencing rationale: page-based storage was load-bearing for pure-query correctness (const T * returns from result_columns.h need pointer stability even single-threaded), so it shipped early as part of Phase A. The remaining five pieces (atomic state, thread-local stacks, multi-producer CAS push, SyncTable, DependencyGraph) are wasted overhead single-threaded — they ship together when the parallel requirement is real. Engine semantics don't change — same queries, same memoization, same dep walks; just thread-safe.

setjmp/longjmp cancellation efficiency — H16's cancel-bail in succeed/fail/stamp_direct guarantees correctness (no canceled result is ever cached), but lets the wrapper finish its compute body using bogus canceled-sentinel values from sub-queries — wasted CPU. Salsa uses panic-unwind to abort mid-compute (salsa/src/cancelled.rs::Cancelled::throw via panic::resume_unwind). C-equivalent: setjmp at db_request_begin, longjmp from db_check_cancel detection points. Future perf optimization; not a correctness fix.

Per-input MIN durability at verify time — slot->durability today is cached MIN-at-succeed over the slot's recorded deps. QueryDep also carries per-dep dep_dur but it's not used at verify time. Phase 8 upgrade: at verify time, compute MIN over LIVE deps only (skipping orphaned-or-reclaimed deps). Sharper bound; marginal LSP gain over the current cached-MIN approach.

Pluggable durability tiers — three hardcoded tiers (DUR_LOW / DUR_MEDIUM / DUR_HIGH). Adequate for typical workspace+library workloads; may bucket too coarsely for monorepos with multiple stability bands (workspace / sibling-team / vendored / stdlib / OS). Salsa supports user-defined tiers. Bump tier count if Phase 5+ analytics show real workload separation that 3 tiers can't capture.

Tree-cache concurrency (NodeCache) — green-tree hash-cons isn't audited for thread safety. Cold-workspace parsing of 1000+ files would parallelize naturally if NodeCache could absorb concurrent inserts. Audit when parallel queries land.

INVARIANTS (not deferrable):

db_input_changed must NOT be called while a db_request is open. Pinned effective_revision assumes inputs are stable for the request's duration. ENFORCED (H24): db_input_changed asserts that rev_control's request bits are zero. db_request_begin asserts revision <= current_revision. db_request_end asserts a request was open. Durability arg asserted in-range for both db_input_changed and db_query_note_input_durability. Single-mutator era's silent footgun is now a hard assert — multi-threaded LSP inherits the same enforcement.

Every wrapper MUST follow the pure-query convention (see engine_internal.h "Result column convention"). Write the result column BEFORE db_query_succeed. Cache-hit path reads the result column. Never write to side-effect storage outside the slot's named result column.

Phase A — Foundation primitives
  A1. Add PagedVec<T> in src/support/data_structure/

Phase B — Schema + contract cleanup (parallel-ish)
  B1. Audit + clean src/db/db.h fossils                                  [DONE]
       — Removed files.node_to_def column + phantom db_query_node_to_def
         call site + db_get_def_for_node (only caller). Deleted the
         FileNodeData / top_level_indices / active_node_type_builder
         stale comment blocks. EVICT_HASHMAP_CLEAR macro removed (now
         unused).
  B2. Migrate slot columns + pointer-returning result columns to PagedVec [DONE]
  B3. Diagnostics: replace TinySpan anchor with DiagAnchor (FileId +    [DONE]
       SyntaxNodePtr-derived).
       — DiagAnchor is a 12-byte struct (file_id:16 + syntax_kind:16 +
         start:32 + length:32). Captured at emit time via
         diag_anchor_of_node(file_local, SyntaxNode*). Diag stays 32 B
         (the union member growth was absorbed by the prior 8-byte
         trailing padding); DiagArg stays 16 B because DiagAnchor's
         4-byte alignment governs the union.
       — db_resolve_anchor (in src/db/getters/diag.c) is the render-
         time resolver: tries syntax_node_ptr_resolve against the
         file's current GreenNode root to rebind a cached diag's
         anchor to its post-edit byte range, falls back to the
         captured (start, length) if no matching node is found.
         db_resolve_span(TinySpan) is kept as a low-level primitive
         used inside db_resolve_anchor and for any future direct-byte-
         range consumer.
       — All sema span_of helpers (check_expr.c, body_scopes.c,
         type_of_expr.c, type_resolve.c) return DiagAnchor. builtins.h
         + builtins.c take DiagAnchor. sema/unused.c uses
         diag_anchor_make for explicit anchor construction.
       — LSP server.c switched range_for_span → range_for_anchor.
       — AstSpan-removal comment in db.h replaced with the DiagAnchor
         design note. Phase-4's "byte-range is sufficient" claim is
         explicitly reversed in the new comment.
       — Resolver allocation pattern: db_resolve_anchor_in takes a
         caller-provided red root; db_resolve_anchor is a one-off
         wrapper that builds + frees the root inline. LSP
         build_publish_params hoists the per-file root build out of
         the diag loop — resolver allocations drop from O(diags) to
         O(files-with-diags), and the red root's child-cache
         amortizes across descents within the same file. Standard
         salsa / rust-analyzer pattern: "trees are cached for the
         scope of a single bulk render pass; resolvers are pure
         functions of (ptr, root)."

Phase B follow-ups (post-B3, pre-Phase C):
  - Deleted dead TinySpan helpers (span_make_range, span_with_file)
    and the dormant db_get_node_span getter. Phase D reintroduces
    db_get_node_span if hover needs a SyntaxNode → TinySpan helper.
  - Removed phantom calls to deleted query-engine APIs in the keep-
    zone: inputs/file.c (QUERY_TOP_LEVEL_INDEX gate-bump → simple
    DUR_MEDIUM bump), inputs/source.c (manual slot reset → engine
    handles via fingerprint + revision), workspace.c (per-slot
    reset on file eviction → engine handles via verify path; the
    db_diags_clear calls are kept). The deleted "../query/
    invalidate.h" header references are gone from all three.
  B4. Audit src/db/getters/; delete derived-state readers, keep input readers [DONE]
       — Removed db_get_namespace_internal_scope (read NAMESPACE_SCOPES
         result; should route through db_query_namespace_scopes). Six
         call sites in src/sema/ and src/ide/ marked with TODO(phase-D)
         for the Phase D consumer rewrite. Source/file/position
         accessors kept as input readers; diag and type formatters kept
         as presentation utilities.
  B5. Rename src/db/setters/ to src/db/inputs/ (cosmetic naming pass) [DONE]
       — Directory renamed via git mv. db.c/db.h/ids.c/engine.h/
         lsp_workload.c docstrings updated; section banners in db.h
         changed from "Setters: …" to "Inputs: …". Function-prefix
         rename (db_set_* / db_create_* → db_input_*) deliberately
         deferred — separate cosmetic pass with much wider call-site
         churn, lands after the build is restored.

Phase B post-cleanup — Resolver collapse [DONE]
  Replaced the two-entry-point resolver surface (db_resolve_anchor +
  db_resolve_anchor_in) with a single DiagResolver struct. Caller
  pattern is stack-allocate → init → use in a loop → free. The
  resolver holds a slot-of-one LRU cache of one file's red root.
  Diag rendering is naturally file-clustered (LSP filters by file;
  CLI iterates per compile_file's collection), so the cache hits 100%
  within an LSP publish and ~100% within a CLI render — both paths
  go from O(diags) tree builds to O(files-with-diags). No HashMap;
  pathological "alternating files" workload doesn't exist for diag
  rendering.

  Surface changes:
    Added (src/db/diag/diag.h, src/db/getters/diag.c):
      DiagResolver (struct), diag_resolver_init, diag_resolver_free,
      diag_resolver_resolve, diag_resolver_print.
    Removed:
      db_resolve_anchor, db_resolve_anchor_in — replaced by the
        resolver.
      db_print_diag(struct db *, …) — replaced by
        diag_resolver_print(DiagResolver *, …); signature change
        forces every caller to thread a resolver, no silent slow path.
      db_collect_diags_all — zero callers, dead code.
    Kept:
      db_resolve_span(TinySpan, …) — low-level primitive (C26 test
        gate binds to it; resolver delegates the final byte-range →
        (line, col) step to it).
      db_format_diag — unchanged; pure template walker.
      db_collect_diags_for_file — unchanged.

  Callers updated:
    src/consumers/driver/build.c — wraps the diag print loop in a
      DiagResolver. Tree builds amortize across all diags from
      compile_file's per-file collection.
    src/consumers/lsp/server.c — build_publish_params now stack-
      allocates a DiagResolver instead of manually hoisting a
      SyntaxTree + red root.

  Adjacent fixes in the same commit:
    - ResolvedSpan typedef hoisted to the top of diag.h (alongside
      DiagAnchor) so it's available to the DiagResolver declarations
      that depend on it.
    - DIAG_ARG_SPAN docstring (diag.h) and %P format-spec comment
      now say "file#N:start-end" — matches actual render output.
      Secondary spans intentionally stay raw (resolution cost not
      worth it on related-info args).
    - Stale "TinySpan-bearing layout" comment on DiagArg trimmed.
    - db.h DiagAnchor design comment + Getters:diag section
      reference DiagResolver, no longer name the deleted APIs.
    - workspace.c eviction-safety comment dropped db_resolve_anchor
      from its reader list.

  Known follow-up [RESOLVED in Pre-Phase-C debt paydown below]:
    - Orphan-DefId diag collection (tools/diag_lifecycle_test.c):
      diags for a slot that got orphaned by an edit linger in
      db.diag_lists. The push-stamp liveness gate inside
      db_collect_diags_for_file now skips orphaned units (D-G10).
      The diag_lifecycle test still can't RUN until Phase D
      re-greens sema (it needs sema_check_module to emit diags),
      so it stays known-failing until then — but the FIX is in.

Pre-Phase-C audit cleanup [DONE]
  An audit across the full src/db/ tree (run before starting Phase C
  parse.c work) found one correctness blocker and several smaller
  follow-ups. Bundled into one commit.

  Critical blocker (was UB-on-first-use):
    - Phase A2's PagedVec migration declared every per-kind slot and
      result column as PagedVec, but db_ids_init kept calling
      vec_init / vec_push_zero (Vec API on PagedVec storage) — every
      paged_init was missing. db_def_set_kind's per-kind row grows
      had the same Vec/PagedVec mismatch. Only top_level_entry's
      four PagedVecs were initialized, and that init lived in
      db_engine_init which was itself never called from db_init.
      Fix: centralized every PagedVec lifecycle in db_ids_init via
      the existing X-macros (paged_init + paged_push_zero for the
      row-0 sentinel where required). Switched db_def_set_kind to
      paged_count + paged_push_zero. Mirrored every paged_init with
      a paged_free in db_ids_free.
    - db_engine_init / db_engine_free are now wired into db_init /
      db_free as the engine lifecycle hooks. db_engine_init owns
      engine-state only (stats counters, running_slots scratch,
      cancel token, top_level_entry routing HashMap); db_engine_free
      runs the deep-free pass (per-slot deps + per-result-struct
      embedded HashMaps) before db_ids_free drops the columns.

  Phantom-symbol cleanup (build-broken references the post-B sweep
  missed):
    - db.c included compact.h + query/collect.h — neither file
      exists. Replaced with query/engine.h.
    - db.c called db_for_each_slot (with a slot_release_visitor
      that freed slot->deps Vecs). Engine reclamation owns this
      now (db_engine_deep_free → reclaim_orphans). Deleted both.
    - db.c called db_register_query_dispatch — the runtime-register
      hook from the pre-X-macro design. The const
      db_engine_recompute_dispatch[] table in engine_dispatch.c
      replaces it; declaration + call removed.
    - compile.c:40 still called db_locate_slot in --profile-loop.
      Replaced with db_diags_clear + db_input_changed(DUR_LOW);
      added a TODO for an eventual public per-slot invalidation hook
      if profile-loop measurements need finer-grained control.
    - db_init's reference to ORE_COMPACT_MIN_THRESHOLD (private to
      engine_compact.c) was removed; engine_compact.c's existing
      zero-fallback covers the default.

  Schema row-grow symmetry:
    - db_create_file / db_create_virtual_file / db_create_namespace
      now grow the parallel PagedVec slot columns
      (ORE_FILES_SLOT_COLUMNS, ORE_NAMESPACES_SLOT_COLUMNS) in
      lockstep with their Vec input columns, so the slot row at
      index N exists by the time the engine routes a query at key N.

  ORE_ENGINE_PRIVATE relaxation:
    - ids.c claims the engine-private guard so sizeof(QuerySlotHot/
      Cold) is available for column init. ids.c is lifecycle
      plumbing, not engine code by call shape; this is a documented
      exception with the rationale at the top of the file. Slot
      struct fields stay opaque to anything outside the engine.

Pre-Phase-C debt paydown [DONE — B2 + G10 + D-HM, "clean engine"]
  Three remaining debt items closed before starting the Phase C parse
  layer. All three land in the keep-zone (workspace, getters/diag,
  inputs/diag, db.h, engine_compact) which compiles + tests now; the
  downstream-dependent validation flips green in Phase D / Phase 8.

  D-B2 — Import cycle safety (Gap G8):
    - Investigation found the audit OVERSTATED this. workspace_resolve_
      import is ITERATIVE (worklist, not recursion) and idempotent: the
      registry check returns the already-resolved NamespaceId on a
      back-edge. A↔B / A→B→C→A cannot OOM or hang by construction. No
      machinery added (would be guarding a scenario that can't happen).
    - Locked in with tools/import_cycle_test.c + `make test-import-cycle`
      (KEEP_ZONE, ASan, `timeout 30` so a regression that reintroduces
      unbounded recursion fails as a timeout, not a hang). PASS.

  D-G10 — Orphan-DefId diag liveness gate:
    - DiagList grew owner_kind (QueryKind) + owner_key (uint64_t),
      stamped at creation in inputs/diag.c diag_list_for_unit.
    - getters/diag.c db_collect_diags_for_file now gates each unit on
      db_slot_is_live(s, owner_kind, owner_key) and skips empty units —
      orphaned slots' lingering diags no longer surface. The engine
      rewrite's liveness primitive (verified_rev == current) is what
      made this fixable; it didn't exist when G10 was first deferred.
    - Compiles clean in keep-zone. Test validation (diag_lifecycle_test
      flips known-failing → green-gate) awaits Phase D sema re-green.

  D-HM — Routing-HashMap entry removal at reclamation:
    - engine_compact.c reclaim_hashmap_kind was zeroing the orphan slot
      but leaving the routing entry pointing at the now-EMPTY row, so
      the routing maps (decl_ast_cache, top_level_entry_cache,
      def_by_identity, resolve_ref_cache) grew monotonically across a
      long LSP session — same leak class as Bug 3, for the routing maps.
    - Fix: two-pass. Collect orphan routing keys (from cold->routing_key)
      during the slot-column walk, then hashmap_remove each in a second
      pass. We iterate the PagedVec not the map, so single-pass would be
      safe, but isolating the mutation keeps the "map shrinks as orphans
      reclaim" contract obvious and sidesteps hashmap_remove's backward-
      shift cleanup. The emptied PagedVec row is stranded (next first-
      call allocates fresh); reclaiming that capacity is a Phase 8
      free-list TODO — the win here is the routing map stays bounded.
    - Locked in with tools/orphan_reclaim_test.c + `make test-orphan-
      reclaim` (KEEP_ZONE, ASan): asserts resolve_ref_cache 64→0→64,
      orphan_reclaimed telemetry, liveness flip, and re-alloc
      consistency. Gates the two-pass removal + deep-free for UAF/leak.
      Full routing-map growth-bound validation (lsp_workload probe)
      needs the full pipeline → deferred to Phase 8.

Pre-Phase-C foundation audit + cheap-and-safe trio [DONE]
  7th audit of src/db (user kept finding bugs across 6 prior passes) —
  3-agent read-only sweep + direct verification of every alarming or
  contradictory claim. VERDICT: foundation is production-sound, no
  correctness blockers. Recorded here so it isn't re-audited an 8th time.

  Verified CLEAN (no action):
    - PagedVec: pointer-stable across growth (only the page-pointer array
      reallocs, live pages never move); correct (page<<10)|slot math;
      symmetric init/free; _Atomic count w/ release/acquire.
    - NO "paged HashMap" needed: every HashMap on struct db stores u32
      row indices as values, never pointers into slot storage — a 2x
      realloc invalidates nothing a caller holds.
    - HashMap backward-shift (Robin Hood) removal is correct (wrap-around
      + occupied bitset verified) and the RIGHT choice over tombstones
      (probe chains stay compact, no accumulation).
    - IDS init/free symmetry: the prior UB-on-first-use bug class is GONE.
      Verified by reading ids.c (resolve_ref/resolve_path freed via the
      X(tbl) macro) AND by ASan-clean db_free across the keep-zone tests.
    - Encapsulation: ORE_ENGINE_PRIVATE is compile-time enforced (#error
      guard), exactly the authorized includers; engine.h leaks no slot
      internals. Single entry points: db_create_def sole DefId minter;
      inputs/ setters bump revision once; stamp_direct rejects RUNNING/
      ERROR. Dispatch X-macro: missing thunk = link error.
    - "sema writes to deleted columns" (agent flag) is OUT OF SCOPE —
      stale src/sema/ (doesn't compile, rewritten Phase D); it confirms
      db correctly removed those columns. ref_count is NOT dead (read by
      sema/unused.c for unused-decl diagnostics) — keep it.

  Cheap-and-safe trio fixed now (~55 LOC, all KEEP_ZONE + ASan-tested):
    - Dead resolve_path schema removed: db.resolve_path struct +
      resolve_path_cache HashMap + their init (ids.c)/init (db.c)/free.
      RESOLVE_PATH was dropped from the QueryKind enum in the rewrite;
      this was orphaned storage. Removal kept ids init/free balanced
      (25/25, ASan-clean).
    - A8 — db_format_type recursion bound: getters/type.c now routes
      through a static format_rec(...,depth) capped at
      DB_FORMAT_TYPE_MAX_DEPTH=16 (mirrors ip_format), rendering "..."
      past the bound. Prevents stack overflow on pathologically nested
      types during diagnostic rendering. Test: tools/format_type_depth
      _test.c (`make test-format-type-depth`) — 2000-deep nest, no crash.
    - A6 — virtual-source name collision: added db.virtual_by_name
      HashMap (interned synthetic-name StrId → SourceId). workspace_admit
      _virtual now rejects a duplicate synthetic name instead of silently
      minting a second source+namespace (db_admit_virtual_source isn't in
      source_by_path). Test: tools/virtual_collision_test.c
      (`make test-virtual-collision`).

  Deferred with rationale (NOT fixed; documented for later):
    - S5 ip_compact tombstones (intern_pool.c) — tombstones only from
      ip_wip_struct_cancel (error-recovery); barely accumulate in normal
      use. ~120 LOC; validate against real workload at Phase 8.
    - S1 db_get_namespace_files O(N) scan — [RESOLVED, see "S1" entry
      below] now backed by a per-namespace reverse index.
    - S6 diag_lists/routing-map monotonic growth — partly mitigated by
      D-G10 + D-HM; full GC ~100 LOC at Phase 8.
    - A4 TinySpan + DiagAnchor file_id is uint16_t → hard 65535-file cap.
      High blast radius (struct repack, every span_make caller), lowest
      probability. ~400 LOC; defer until file counts demand.
    - PagedVec slot-row stranding on orphan reclaim — routing maps shrink
      (post-D-HM), slot columns don't. Slow leak, not corruption. Phase 8
      free-list; needs real query bodies to validate.
    - comptime_call_cache — intentional placeholder for future comptime
      work (workspace.h:53); leave as documented groundwork.

Known IP architectural debt (NOT in this commit; flagged for later)
  - ip_compact is a deferred stub returning false. ip_remove creates
    IP_TAG_REMOVED tombstones via ip_wip_struct_cancel
    (error-recovery only); without compaction they accumulate.
    Bounded by error-recovery rarity. Revisit when (a) parallel
    queries land and tombstone density becomes measurable, or
    (b) a heavy wip-cancel workload appears.
  - ip_wip_struct accepts captures / n_captures and ignores them
    via (void) casts. Groundwork for the future comptime-arg
    dispatch chunk; signature is intentionally preserved.

Phase C.0 — Input-dependency substrate [DONE]
  Implementing the first REAL query body (file_ast) surfaced a
  foundational engine gap: the engine had the durability skip-
  optimization + fingerprint early-cutoff, but NO input->query
  dependency — the correctness substrate those optimizations sit on.
  file_ast read source text via an untracked getter, recorded zero
  deps, and so could never be invalidated (db_engine_verify's dep walk
  over an empty dep list returned true). Hidden until now because Phases
  0/1 validated the engine against stubs that read no real inputs.

  Grounded in salsa ("inputs are queries"; durability is only the skip-
  optimization; early cutoff is fingerprint backdating), the fix:
    - SOURCE_TEXT input query kind (keyed by SourceId), Vec-indexed slot
      whose fingerprint = source content hash. recompute_SOURCE_TEXT is
      a no-op (inputs are SET, never computed).
    - db_input_set(ctx, kind, key, fp, dur) — salsa's `set`. Marks the
      input slot DONE with fp + tier at the current revision.
    - create_source_row + db_set_source_text call db_input_set so the
      slot fingerprint tracks the source hash; readers (db_query_file_text)
      record a dep via the begin->CACHED path, so verify invalidates them
      per-source. Durability tier threads through for the skip fast-path.
    - Decision: model B (pull early-cutoff firewall), NOT push-stamp.
      Push-stamp (stamp_direct/stamped_rev/verify short-circuit) RETIRED
      — it was never load-bearing (orphan reclamation + diag liveness key
      off verified_rev). decl_ast/top_level_entry are dep-tracked queries
      with content-hash fps.

  ALSO FOUND + FIXED (same stub-hidden class): SoA slot-sentinel
  off-by-one. Vec columns push a row-0 sentinel; the files/namespaces
  PagedVec SLOT columns did NOT, while FILE_AST/NAMESPACE routing indexes
  slots by the sentinel-inclusive local idx. So FILE_AST routing returned
  false for every real file (never exercised — all FILE_AST tests use the
  deleted old API). Fix: slot columns push the row-0 sentinel too (files,
  namespaces, sources), restoring Vec-idx == slot-idx. SoA invariant now:
  row 0 reserved on EVERY column, Vec and slot alike.

  Cruft retired: has_untracked_read (already gone), phantom db_revalidate
  comment -> db_engine_verify, durability documented as skip-only.

  Gate: tools/input_incremental_test.c (`make test-input-incremental`,
  KEEP_ZONE + ASan) — edit A -> file_ast(A) recomputes; unrelated B ->
  cache-hit (per-source, not per-tier); byte-identical -> no-op. This is
  the Phase-0 incremental coverage that only ran against the OLD engine,
  now green against the new one. Full keep-zone gate green.

Phase C.0a — Substrate audit + wart remediation [DONE]
  3-agent deep review of this session's work (engine core / input
  substrate+parse / schema+ids+compaction) + direct verification of every
  "correctness bug" claim (agents overstate). Real findings fixed:
    W1 (correctness, latent): dep_index_key(kind,key)=(kind<<56)|
      (key&0x00FF..FF) truncates the key's top 8 bits, so two distinct
      deps whose keys differ only there collide in a frame's dep_index;
      the old code overwrote one dep's fp in place → lost dep → missed
      invalidation (only reachable >2^24 entities, but the same silent-
      truncation class as past bugs). Fix: dep_index is now an ADVISORY
      hint — confirm (kind,key) on a hashmap hit, append on collision.
      Collision-safe regardless of key encoding. Gate: tools/dep_dedup_
      test.c (`make test-dep-dedup`) forces a bit-56 collision, asserts
      both deps survive.
    W2+W3 (design): input-vs-derived kinds were flattened with input-ness
      enforced only by convention. Now each ORE_QUERY_KINDS entry is
      tagged INPUT/DERIVED; db_query_kind_is_input() (generated from the
      tag) is asserted in db_input_set (input-only — closes the back-door
      where a derived slot could be set bypassing begin/succeed) and
      db_query_succeed (derived-only). Salsa's ingredient-type distinction,
      enforced in ore's X-macro style.
    W4: dropped dead slots_index_hot/cold from ORE_NAMESPACES_SLOT_COLUMNS
      (TOP_LEVEL_INDEX removed; nothing routed to it) — like resolve_path/
      files.tokens.
    W5: db_query_file_text's dead COMPUTE path → assert (an input slot is
      always pre-set via db_input_set; reaching COMPUTE is a contract
      violation, not a value to fabricate).
    W6: documented that QUERY_SOURCE_TEXT (and future INPUT kinds) are
      intentionally excluded from orphan reclamation (inputs are
      authoritative; reclaiming a live input would drop its fingerprint).
    W7: reclaim_vec_kind/reclaim_hashmap_kind loop from row=1 (structural
      sentinel skip, matching reclaim_type_slots).
    W8: engine_verify.c contract docs — per-dep fingerprint is the
      authoritative comparand (verify ignores the dep's verified_rev,
      which is what makes INPUT deps work uniformly); durability is the
      skip-optimization only.
  Verified NOT bugs (recorded so they aren't re-raised): green-root
  "double-free" on root==old (node_cache RETAINS on a hash-cons hit, so
  the release is balanced — the agent's proposed `old!=root` guard would
  LEAK); db_input_set ordering (verify is fingerprint-based; input_
  incremental_test proves it); reclaim row=0 (correct via sentinel-always-
  EMPTY; row=1 is robustness only).
  Deferred (each with a reason): file_ast prescan + malloc token vec
  (correctness-neutral), lex-error detail (diag-quality pass), line_starts
  multi-thread coherence (parallel phase), input-slot eviction (Phase 8).
  Full keep-zone gate green under ASan (dep-dedup, input-incremental,
  import-cycle, orphan-reclaim, format-type-depth, virtual-collision,
  intern-pool).

Phase C.0b — line_index purification [DONE]
  Pre-C.1 assessment found one real purity deviation: file_ast wrote
  files.line_starts as a SECOND output (outside its result column) — the
  lone violation of "no query writes SoA outside its result column."
  Fixed by extracting LINE_INDEX as its own pure DERIVED query:
    - LINE_INDEX(file): tagged DERIVED in ORE_QUERY_KINDS; Vec-indexed
      slot (files.slots_line_index_hot/cold); result column =
      files.line_starts. Depends ONLY on SOURCE_TEXT (line starts are a
      byte scan — no lexer), so it's parallel to file_ast, not downstream.
      Matches lex_newline semantics (\n, \r\n, lone \r). Result hosted in
      files.arenas[fid] (line_index is now its sole owner; file_imports
      in C.1 must coordinate arena use).
    - file_ast is now PURE: result = green_roots only. It no longer
      touches files.arenas or files.line_starts; the lexer's line_starts
      vec is malloc scratch, discarded after parse.
    - getters (position.c, diag.c) unchanged — they still read
      files.line_starts (now populated by LINE_INDEX, pulled by the
      driver/test, same as file_ast). They stay pure reads.
    - ASan caught a leak: adding a DERIVED kind requires wiring it into
      db_engine_reclaim_orphans (else its slots' malloc-backed deps are
      never deep-freed). LINE_INDEX added to the reclaim walk.
  HYGIENE FOLLOW-UP (deferred): db_engine_reclaim_orphans is a MANUAL
  per-kind list — a new DERIVED kind silently leaks until added (just hit
  this). Could be X-macro-driven (iterate ORE_QUERY_KINDS, skip INPUT),
  but the three storage shapes (Vec-indexed / HashMap-routed / per-DefId)
  make a uniform walk non-trivial. Revisit if more kinds are added.
  Gate: tools/line_index_test.c (`make test-line-index`) — LF+CRLF
  offsets, position.c integration, per-source recompute. Full keep-zone
  gate green under ASan.

Phase C.1a — Trivia stability + scratch hygiene [DONE]
  Pre-continue review found two issues:
    Fix A (correctness/perf — contract C3): decl_ast's fp was
    green_node_hash_of(subtree), which folds ALL children incl trivia
    tokens (green.c green_node_compute_hash; parser attaches trivia
    INSIDE the owning node) → trivia-SENSITIVE → a whitespace/comment
    edit changed a decl's fp → its downstream (sema) recomputed. The old
    engine used a trivia-excluding structural hash; the rewrite lost it.
    Added green_structural_hash (green.c / node_cache.h): recurses the
    subtree folding node kinds + NON-trivia token identities (hash-consed
    token pointer = kind+text, so renames still recompute — C4), SKIPS
    trivia. Position-independent. decl_ast now uses it. file_ast keeps
    green_node_hash_of (position-SENSITIVE on purpose, so decl_ast re-runs
    to refresh its position-dependent ptr while its structural fp stays
    stable). Net: a comment/whitespace edit reparses file_ast + re-scans
    line_index + cheaply re-derives decl_ast ptrs, but sema cuts off.
    Recursion is bounded by what the parser produced (no deeper than the
    parser's own descent). Gate: tools/trivia_stable_test.c
    (`make test-trivia-stable`). NOTE: my parse_incremental test had only
    covered C2/C20 (position-independence), never C3 (trivia) — gap closed.

    Fix B (DoD): file_ast's token stream + the lexer's scratch line_starts
    were malloc-backed (vec_init/vec_free per parse — churn on the reparse
    hot path). Moved to db.request_arena (the designated request-scoped
    scratch; bump-allocated, reclaimed at db_request_end) per the lexer.h
    contract. Fixed safe capacities (arena vecs can't grow): line starts
    ≤ len+1; unified stream ≤ 4*len (trivia+real ≤ len + layout virtuals
    ≤ ~3*len). The ParseError list stays malloc (bounded, freed).

  Trivia-stability summary (answering "are we stable across trivia?"):
  file_ast ALWAYS reparses on any byte change incl whitespace (unavoidable
  pre-incremental-parsing; same as rust-analyzer). But the per-decl
  structural fp is now trivia-insensitive, so the EXPENSIVE downstream
  (type/sema) cuts off — only reparse + line_index + cheap decl re-derive
  run. That's the contract-C3 behavior.

Phase C.1 — Parse bodies (model B, on the proven engine) [DONE]
  C1. file_ast: DONE — parse (lex->layout->parse_file_green), green-root
      release on reparse, line_starts in the per-file arena, lex+parse
      diag drain to the FILE_AST unit, SOURCE_TEXT input dep.
  C2. decl_ast: DONE (Phase C.1a) — position-independent structural-hash
      fp, file_ast dep.
  C2b. top_level_entry: DONE — the per-name firewall. slot_alloc + guard
      (key (nsid<<32)|name.idx); records FILE_SET(nsid) + file_ast(file)
      per scanned file; walks each file's top-level node children, interns
      the decl name via a per-kind switch (FnDef_name/StructDef_name/…),
      first-match wins; result TopLevelEntry{name, node_ptr, meta=0}, fp =
      green_structural_hash(decl) (position-independent firewall). meta=0
      with a TODO(phase-D) for visibility (the `pub` modifier consumer
      isn't landed). Moved out of stubs.c into parse.c.
      FILE_SET input kind (the architecturally-significant piece): a
      per-namespace input slot whose fp folds each file's id on add
      (db_create_file via db_fp_combine; seeded empty by
      db_create_namespace). Without it, top_level_entry(name) caching
      NOT_FOUND at rev N would cache-HIT on the old file set's file_ast
      deps when a later file defining `name` joins → stale NOT_FOUND; the
      coarse DUR_MEDIUM bump alone can't give per-namespace precision.
      INPUT kind (no-op recompute thunk), Vec-routed by nsid, excluded
      from orphan reclaim (authoritative, like SOURCE_TEXT).
      Gate: tools/top_level_entry_test.c — lookup/miss, the sibling-edit
      firewall (foo fp stable across a preceding-decl shift), and the
      FILE_SET file-add correctness case.
  C3. file_imports: DONE — walks the whole tree for @import("path")
      SK_BUILTIN_EXPR sites (the ONLY import form; the vestigial
      SK_IMPORT_DECL kind + ImportDef wrapper were never parser-emitted
      and were REMOVED — ore_kind_is_decl_node upper bound moved to
      SK_DESTRUCTURE_DECL). Path interned (quotes stripped, mirroring
      sema/builtins.c), site = SyntaxNodePtr of the builtin; fp folds path
      StrIds in document order (add/remove/reorder shifts it; unrelated
      edit leaves it stable). Result body is a STANDALONE malloc in
      files.imports (NOT the per-file arena — line_index owns + resets
      that; malloc is also denser, a no-import file stores {NULL,0}):
      free-old-then-malloc-new on recompute, EVICT_FREE_FILEARRAY on
      evict, and a teardown free in db_ids_free (the last live body).
      Gate: tools/file_imports_test.c — extraction + fp firewall, ASan
      confirms no leak/UAF across the malloc lifecycle.
  C.1c doc refresh: lexer.h/token.h purged of stale `mod->arena` /
      `mod->trivia_map` / `db.names.<kw>` references → per-file arena +
      QUERY_LINE_INDEX, lossless trivia-as-green-children, and the
      source-byte contextual-keyword compare (tok_str_eq / TOK_IS).
  Full keep-zone gate green under ASan — all 11 KEEP_ZONE_SRCS targets
  (dep-dedup, format-type-depth, import-cycle, input-incremental,
  line-index, orphan-reclaim, parse-incremental, trivia-stable,
  top-level-entry [new], file-imports [new], virtual-collision) +
  syntax-kind / ast-wrappers / parser-green. NOTE: `make all` still fails
  in src/sema|ide|compiler on stale db/query/*.h includes — those are the
  Phase D consumers, out of scope; the keep-zone is the gate.

Phase C.2c — AstId revival: per-namespace items index [DONE]
  Audit finding: the founding axiom "per-entry queries from day 1, no
  aggregating index" (line 110) left top_level_entry re-walking the whole
  namespace per NAME and gave NO way to ENUMERATE a namespace's names —
  yet db.h claimed it "the single source of truth." Enumeration consumers
  (namespace_type = module-as-struct, completion) are Phase D, so the
  shape was corrected now, before they build on it. Not a bug (the firewall
  worked + was tested) — a missing layer + RA-fidelity gap. The "index
  stales" worry conflated an IMPERATIVE side-table (stales) with a DERIVED
  query (engine recomputes — can't stale).
    Revived AstId (ids.h: content-addressed hash(kind,name), reparse-stable;
    a flat-AST-era fossil the green-tree rewrite orphaned). CONFINED to the
    named top-level item layer — decl_ast / node-level queries stay on
    SyntaxNodePtr (names don't exist there); dup-name is first-wins, same as
    before. This is RA's AstIdMap pattern (stable id + per-reparse map to
    current ptr) that ore had dropped.
  NAMESPACE_ITEMS (DERIVED, nsid-keyed): walks the file-set ONCE producing
    NamespaceItem{id=ast_id_compute(kind,name), name, file, ptr (current),
    struct_hash, meta}[]. Deps FILE_SET(nsid) + file_ast(each). fp folds
    (AstId + struct_hash) per item in doc order — POSITION-INDEPENDENT
    (pure shift → backdates; rename/add/remove/content-edit → flips).
    Result body = standalone malloc in namespaces.items (FileArray; like
    file_imports): free-old/malloc-new on recompute, teardown free in
    db_ids_free, NO evict handler (no per-namespace eviction path). Serves
    enumeration directly. New-kind wiring: engine.h X-macro, dispatch
    thunk, engine.c nsid-Vec route, result_columns accessor, reclaim walk.
  top_level_entry → thin READER over NAMESPACE_ITEMS. Deps NAMESPACE_ITEMS
    + (on match) file_ast(item.file). The file_ast dep is LOAD-BEARING: on
    a pure shift the index recomputes (fresh ptr in its column) but
    BACKDATES (fp stable), so the index dep alone wouldn't re-verify this
    slot → stale ptr; the file_ast dep forces a recompute that re-reads the
    fresh ptr while our struct_hash fp stays stable (same mechanic as
    decl_ast). Result gained an AstId `id`; dropped the direct FILE_SET dep
    (now transitive). The full RA end-state (drop node_ptr, materialize via
    an AstId→ptr query; re-key def_identity/resolve_ref on AstId) is Phase
    D, landing with those consumers.
  Cleanups: shared ast_string_literal_text (ast_expr.c) kills the @import
    quote-strip duplication across file_imports + sema/builtins.c (the
    builtins.c half is ungated — pre-existing Phase-D non-compile). Doc
    honesty: db.h top_level_entry "single source of truth" → "per-name
    reader; enumeration is NAMESPACE_ITEMS"; file.c FILE_SET combine noted
    order-SENSITIVE (deterministic add order makes it fine).
  Gate: tools/namespace_items_test.c (enumeration, index firewall =
    trivia-stable fp + current ptr + stable AstId, content/rename fp
    changes) + top_level_entry AstId-stability assert. Full keep-zone green
    under ASan — 12 KEEP_ZONE_SRCS targets (+ namespace-items) + syntax-kind
    / ast-wrappers / parser-green.

S1 — per-namespace file reverse index [DONE]
  db_get_namespace_files scanned the dense files.module_id column O(all
  files in the workspace) per call, then copied non-evicted matches to
  request_arena. With file-as-namespace this made db_query_namespace_items
  (and 8 other callers) pay an O(total-files) scan on every recompute — an
  LSP per-keystroke cliff. Replaced the scan with a per-namespace reverse
  index: namespaces.member_files, a Vec<FileId> column (input-side,
  append-only — same class as file_by_source, NOT a query). A Vec (not a
  FileArray) because this is append-only and wants amortized-O(1) push, vs
  FileArray's realloc-copy-per-add which fits only the wholesale-replaced
  query-result columns. Maintained in file_set_add (the shared "file joined
  namespace" hook, covers real + virtual files); vec_init'd per row in
  db_create_namespace (vec_push_zero leaves element_size 0); inner-vec_free
  per row in db_ids_free. The getter now reads member_files (O(ns files)),
  filters evicted at read (now defensive — see remove-on-evict below), and
  keeps its EXACT contract (request_arena FileId[], evicted excluded) so all
  9 callers are untouched. Phase D's scope-layer inherits this index. Gate:
  tools/namespace_files_test.c (membership, multi-file namespace, evicted
  exclusion, empty/sentinel → NULL). Full keep-zone green under ASan — 13
  KEEP_ZONE_SRCS targets (+ namespace-files).

FILE_SET remove-on-evict [DONE]
  Was deferred to Phase 8 ("db_fp_combine isn't reversible"). The S1
  member_files index makes it clean: db_namespace_remove_file (file.c)
  order-preserving-removes the fid from member_files, then RECOMPUTES the
  FILE_SET fp by folding the survivors from the empty-set base db_fp_u64(0)
  — which reproduces exactly what the add-path's incremental combine would
  yield (combine is just a fold), so the add path is UNCHANGED (O(1)) and
  only removal recomputes (O(ns files)). Wired into the per-file loop of
  workspace_did_evict_source (member_files/FILE_SET are namespaces columns,
  untouched by the files-evict X-macro). Eviction was already read-correct
  (the getter filtered evicted + NAMESPACE_ITEMS re-resolved via the
  evicted file's file_ast dep → FINGERPRINT_NONE); this additionally keeps
  the FILE_SET fp membership-exact and stops member_files retaining dead
  entries — hygiene, with real payoff alongside multi-file modules. Gate:
  tools/evict_membership_test.c (fp drops a member's contribution on evict,
  == fold-of-survivors, empty == seed). Full keep-zone green — 14 targets.

db/ audit remediation [DONE]
  A 3-agent sweep (complexity / DoD-SoA / granularity / hygiene) found the
  implemented layers sound + maximally granular; three live-code warts +
  stale comments fixed:
  1. REMOVED decl_ast (QUERY_DECL_AST). It was vestigial — no live consumer
     (only tests pulled it), superseded as the per-decl content-firewall
     handle by AstId-keyed top_level_entry (C.2c). Its ptr-keyed slot key
     churned on every range-shifting edit; removing it deletes that churn +
     a query kind. parse_incremental / trivia_stable retargeted to
     top_level_entry (same C2/C20 + C3 coverage). The remaining ptr-keyed
     churn source is def_identity (a stub) — Phase D builds it AstId-keyed
     (nsid<<32 | astid from top_level_entry.id), not ptr_hash.
  2. db_collect_diags_for_file fast-path: DiagList gained collect_file (the
     file all its diags anchor in; DIAG_LIST_MULTI_FILE for the rare
     cross-file unit). Per-file collection skips a non-matching single-file
     unit with a u32 compare instead of a db_slot_is_live lookup + items
     scan. Chose this over a HashMap<file→rows> index to NOT add another
     monotonic structure (the audit flagged we have several); still O(all
     units) outer but the per-unit work is gated.
  3. Reclaim exhaustiveness guard: db_engine_reclaim_orphans now dispatches
     through reclaim_one_kind, a switch over every QueryKind wrapped in
     `#pragma GCC diagnostic error "-Wswitch-enum"` (build is -Wall, not
     -Werror) — a new DERIVED kind can't compile until its reclaim is wired.
     Closes the hand-maintained-list footgun (hit twice: LINE_INDEX,
     NAMESPACE_ITEMS). Type-slot kinds reclaim together (reclaim_type_slots,
     once) so their cases are no-ops that only satisfy the guard.
  4. Comment cleanup: dropped retired push-stamp/stamped_rev refs (engine.c,
     engine_internal.h); fixed the db.h files-belonging-to-module note to
     reflect member_files + S1; corrected the db_create_virtual_file
     TOP_LEVEL_INDEX-gate comment to the current revision-bump-skip.
  Confirmed NOT warts: the active engine is strictly PURE — the "run X for
  its side effect" pattern lives only in dead src/sema/ (db_query_ensure on
  the deleted QUERY_TOP_LEVEL_INDEX; those files don't compile, rewritten in
  Phase D). namespace_items re-walking its namespace per edit is already
  minimal (file-as-namespace → ~1 file). Full keep-zone green — 14 targets.

Phase D1 — name-resolution layer (scope.c) [DONE]
  Replaced the three name-layer stubs with real bodies in a new
  src/db/query/scope.c (the stubs were deleted in the same change — scope.c
  + stubs.c are both globbed into the build, so a duplicate definition would
  link-error).
  - def_identity(nsid, AstId) → canonical, reparse-STABLE DefId. THE central
    decision: it MINTS a DefId (db_create_def + db_def_set_kind) inside the
    query — the monotonic INTERNING pattern (like pool_intern → StrId), NOT
    the dead "side-effect" hack: it's identity-keyed + idempotent and the
    result (DefId) lives in the slot's own result column. Sound because it's
    AstId-keyed: key = (nsid<<32 | astid.idx), fully reversible (recompute
    reconstructs args from the key → dropped the def_identity.keys column +
    re-keyed engine_dispatch.c off ptr_hash). Same decl → same slot → mint
    once, reuse forever (recompute reads its own result column). fn→struct
    kind-change → new AstId → new slot → new DefId automatically. Recovers
    name/kind from NAMESPACE_ITEMS (added a `kind` field to NamespaceItem).
    fp = db_fp_u64(DefId.idx) (STABLE) → downstream keyed on the DefId cuts
    off when def_identity merely re-verifies.
  - namespace_scopes(nsid) → builds the `internal` scope (name→DefId, one
    binding per NAMESPACE_ITEMS item via def_identity), parented to the
    primitives scope; `exported` deferred to NAMESPACE_TYPE (D2) → NONE.
    Reuses the ScopeId across re-runs; rewrites the decl_pool range. fp folds
    (name, DefId) → stable across a body edit (same names+DefIds) so
    resolve_ref cuts off; flips on add/remove/rename.
  - resolve_ref(scope, name) → walks the scope's bindings then parents to
    the primitives scope. Deps: namespace_scopes(scope owner) + the parent's
    resolve. fp = stable DefId; miss → NONE.
  - Schema: DeclEntry changed {name, node_ptr} → {name, DefId} — storing the
    stable DefId is now SAFE (AstId-keyed def_identity ⇒ DefId never goes
    stale) and avoids caching a position-dependent ptr behind a position-
    independent scope fp; primitives init (db.c) stores each prim's DefId
    directly (resolve_ref needs no primitives special-case).
  Gate: tools/{def_identity,namespace_scopes,resolve_ref}_test.c. Full
  keep-zone green under ASan — 17 KEEP_ZONE_SRCS targets + smoke. (Sema/ide
  consumers still don't compile — D3.)

D1 follow-up — NAMESPACE_ITEMS made a membership signal + def_identity #1/#4 [DONE]
  Post-D1 review found the name layer (def_identity, namespace_scopes)
  depended on NAMESPACE_ITEMS, whose fp folded struct_hash (content) — so a
  body edit recomputed the whole name layer (O(K·N) cheap churn/keystroke),
  even though identity was unchanged. Fix (same code, one refactor):
  - NAMESPACE_ITEMS fp is now a MEMBERSHIP fold of AstIds only (no
    struct_hash); items are SORTED by AstId (canonical → reorder-stable fp +
    def_identity binary-searches, O(log N)). struct_hash dropped from
    NamespaceItem; top_level_entry now computes it itself (it already holds
    file_ast). Net: content/shift/reorder edits leave the index fp STABLE
    (name layer cuts off); only add/remove/rename flips it. top_level_entry
    keeps the per-decl content firewall (computes the structural hash on the
    resolved node).
  - def_identity: binary-search the sorted items (was O(N) scan → O(log N),
    cold scope build O(N²)→O(N log N)); and DROPPED its stale syntax_ptr
    write (#1) — it was mint-time-only and can't stay fresh behind the
    membership fp; current location is top_level_entry, syntax_ptrs[def]
    left {0} to fail loud not stale.
  Decided NOT to do: generational arena / compacting PagedVec for the
  identity pools. After D1 every slot is stably keyed (no per-keystroke
  churn) and the residual growth (defs rows per rename/kind-change; decl_pool
  per membership change) is slow + session-bounded — same as RA/Zig, which
  don't GC their intern/def pools. The Phase-8 GC items (slot free-list,
  ip_compact) stay deferred, additive if a real long-session profile ever
  demands. Gate: namespace_items_test updated to membership semantics; full
  keep-zone green under ASan.

Tracked follow-ups (D1 review — deliberately NOT fixed now)
  - SORT-KEY ASYMMETRY: NAMESPACE_ITEMS is sorted by AstId, so def_identity
    binary-searches (O(log N)) but the NAME lookups (top_level_entry,
    resolve_ref's scope scan) stay O(N). The name path is plausibly hotter
    once D2/LSP land. Don't fix speculatively — DECIDE AT D2 with the real
    hot path: sort by name instead, or keep a second name index. Irrelevant
    at today's tens-of-decls-per-file N.
  - ASTID 32-BIT COLLISION (correctness, low-probability): AstId =
    FNV-fold(kind,name) into 32 bits; def_identity is keyed by it, so two
    distinct decls in ONE namespace that collide would SILENTLY alias to one
    DefId (~N²/2³³ per namespace — negligible for small N, grows with size).
    No clean fix now: widening AstId to 64-bit breaks the reversible packed
    key (nsid<<32|astid) → would force the def_identity.keys column back; a
    runtime assert is production-unsafe (crash on valid colliding code).
    Durable fix at production-hardening: 64-bit AstId / 2-level key + a
    proper collision DIAGNOSTIC (needs the Phase-D diag layer).
  (Micro-opts intentionally ignored: top_level_entry's per-recompute red-
  tree alloc — addable per-request cache if profiled; namespace_items'
  re-sort each recompute — intrinsic to doc-order→astid-order, cheap at N.)

D2.0 — semantic-kind classification fix [DONE]
  Found while planning D2: ore is expression-oriented, so `Foo :: struct{}`
  parses SK_CONST_DECL{value=nameless SK_STRUCT_DECL} and `f :: fn(){}` binds
  SK_LAMBDA_EXPR (parse_decl.c / parse_expr.c parse_aggregate_expr). The
  NAMESPACE_ITEMS walk recorded the WRAPPER kind, so def_identity classified
  every bind KIND_CONSTANT/KIND_VARIABLE — nominals/fns never reached
  db.fns/db.structs and defkind_of's SK_FN_DECL/SK_STRUCT_DECL arms were dead.
  Fix (parse.c, in the existing single walk — no re-walk): peek the RHS value
  of a `::`/`:=` bind (modifiers are sibling tokens, not wrappers, so the value
  node's kind is already semantic); item.kind = semantic kind, AstId =
  ast_id_compute(kind, name) so a struct→enum retype is a clean new-DefId (no
  db_def_set_kind "kind fixed" assert). meta/pub deferred to D2.2 (where
  namespace_type consumes it + can be tested against real pub syntax). Gate:
  tools/classify_test (struct/fn/const kinds + struct→enum retype→new DefId
  KIND_ENUM); full keep-zone green.

  D2.0b consolidation: collapsed the initial two-step (decl_name_of for name +
  decl_semantic_kind for SyntaxKind + scope.c defkind_of for SyntaxKind→DefKind)
  into ONE decl_classify(child,&name)→DefKind that casts the wrapper once and
  classifies straight to DefKind. NamespaceItem.kind is now DefKind (was
  SyntaxKind); the lambda→SK_FN_DECL normalization hack and defkind_of are gone;
  def_identity uses item->kind directly. Single source of truth — removed the
  two-switch sync hazard + per-decl double-cast. Functionally equivalent (AstId
  values are now DefKind-keyed but stable/flip behavior is identical; AstIds are
  recomputed each parse, never persisted). Touched parse.c/db.h/scope.c; full
  keep-zone green.

D2.1 — declared-interface type layer (type.c) [DONE]
  New src/db/query/type.c + type_layer.h: type_of_def, fn_signature,
  resolve_type_expr, build_fn_type, build_struct_type, build_enum_type, the
  NodeTypeBuilder. Ported from sema/{type_resolve,fn_signature,type_of_def}.c;
  dep plumbing rewired onto D1. Stubs for type_of_def + fn_signature deleted.
  - Preamble: a decl's CURRENT location = top_level_entry(nsid, name) (the
    content firewall dep) + read files.green_roots[local] RAW (NO file_ast dep —
    that would defeat the firewall: a sibling edit would re-run it). type_of_def
    dispatches on db_def_kind. Primitives (i32, …) short-circuit via
    db_primitive_type_for BEFORE the guard — they have no per-kind slot.
  - type_of_def(fn) delegates to fn_signature (deps on it, not top_level_entry)
    → cuts off on body edits. Nominals: ip_wip_struct publishes wip.index into
    the type cell before the field loop; on_cycle = type_of_decl_read so a
    self-ref (`Node{next:^Node}`) reads the in-progress index. Typed binds
    resolve the annotation (no RHS check — D2.4); inferred binds → IP_NONE (D2.4).
  - fps: fn_signature/type_of_def(fn) = fn-type IpIndex (structural). Nominals =
    combine(IpIndex.v, ⊕ field/variant content). NodeTypeBuilder fp folds ONLY
    type values in push order (#6: position-independent — drops the
    syntax_node_ptr_hash term, so the body fp is trivia-stable like parse-layer fps).
  - #5 decided: kept the O(N) resolve_ref scope scan — it's a memoized query so
    each name is scanned once then cache-hits; amortized, denser than a per-scope
    HashMap at file-scope N. Closes the sort-key-asymmetry follow-up.
  - Schema: added FileId to TopLevelEntry; type_of_decl_node_types_write (free-old
    discipline) added to result_columns.h.
  Gate: tools/type_of_def_test (struct/self-ref/fn/typed-const types; nominal
  stable across sibling edit; field edit flips fp) + full keep-zone green.

  INTERN-POOL AUDIT (interwoven) — found: ip_wip_struct never dedups (FRESH
  index every call + unconditional new zir bucket entry); enums via ip_get
  returned the stale existing payload; combined with build_struct_type's
  fieldless ip_get path a fielded→fieldless struct returned a STALE 2-field
  type — a latent correctness bug, not just churn. Pulled the fix forward into
  D2.1b (below) rather than deferring.

D2.1b — nominal types: stable inline identity + field/variant data in db pools [DONE]
  Root cause: the intern pool's chained arena is immutable-friendly, but nominal
  field/variant lists are recompute-churning data — wrong storage. Fix (RA/Zig +
  DoD): the pool keeps only immutable nominal IDENTITY; mutable field/variant
  lists move to recompute-friendly db pools. Safe (audit: ZERO keep-zone readers
  of struct/enum field payloads from an IpIndex — field access is D2.4).
  - intern_pool: IP_TAG_STRUCT_TYPE / IP_TAG_ENUM_TYPE now INLINE-encoded
    (items_data = zir_node_id) — encode_items_data + ip_key_internal inline-
    decode; dropped the arena encode_payload/ip_key cases + IpStructPayload/
    IpEnumPayload; IpKey.struct_type/.enum_type slimmed to {zir_node_id}. hash/eql
    already zir-only. REMOVED ip_wip_struct/_finish/_cancel (kept ip_wip_fn_type —
    structural fns). Inline structs/enums never hit the wip sentinel.
  - db: AggregateFieldEntry{name,type} (structs AND unions) / EnumVariantEntry
    {name,value}; db.aggregate_field_pool / db.enum_variant_pool (Vec);
    structs.(field_lo,field_len) + unions.(field_lo,field_len) /
    enums.(variant_lo,variant_len); by-value accessors db_aggregate_field_count
    / _at / _type (struct|union — never hand out a pool pointer) + db_enum_variants
    (keyed by def == zir). Union members ARE stored in the
    shared pool (same as structs); only struct-specific per-annotation HOVER
    types are struct-only (unions have no type_result column) — member access is
    symmetric.
  - type.c: build_struct_type/build_enum_type → ip_get(IPK_*,{zir}) STABLE +
    publish cell (self-ref anchor) + resolve fields into a request-arena scratch +
    bulk-append to the pool AFTER the loop (nested type_of_def appends to the same
    pool, so deferring keeps our range contiguous) + stamp (lo,len). A failed
    field stores type=IP_NONE (struct stays a valid nominal — no whole-type
    cancel). The fp content-fold is now load-bearing (stable index).
  - tests: type_of_def_test asserts nominal IpIndex STABLE across field-type AND
    fielded→fieldless edits + db_struct_fields name→type + mutual recursion A<->B;
    intern_pool_test struct/enum inline-identity tests; DELETED obsolete
    cycle_struct_test/cycle_union_test (pre-D1 architecture: sema.h, top_level_index).
  - Deferred to D2.2: the new pools strand ranges on recompute (decl_pool pattern)
    → their compaction lands with ip_remove/ip_compact. Bonus: nominal arena
    churn is GONE, so ip_compact is simpler.
  Gate: test-type-of-def + test-intern-pool + full keep-zone green under ASan.

D2.2 — meta/pub + namespace_type (inline+pool) + field-pool compaction [DONE]
  - meta/pub: decl_meta_of(child) (parse.c) scans the decl node's green TOKEN
    children AFTER the bind op (SK_COLON_COLON/:=/:) for modifiers (real syntax
    is `Name :: pub <rhs>`, confirmed in examples) → DefMeta (pub→VIS_PUBLIC,
    comptime/distinct/abstract/scoped/linear/named bits). meta folded into the
    NAMESPACE_ITEMS membership fp (NOT the AstId): a pub/distinct toggle flips
    the fp so def_identity refreshes defs.meta + namespace_type re-filters, but
    the DefId stays stable (same decl). classify_test covers pub→VIS_PUBLIC.
  - namespace_type: converted IPK_NAMESPACE_TYPE to INLINE (items_data = nsid),
    mirroring D2.1b struct/enum — hash/eql nsid-only, dropped IpNamespaceTypePayload
    + arena encode/decode, slimmed IpKey.namespace_type to {nsid}. The exported
    member list (name→DefId, DeclEntry) lives in db.namespace_field_pool +
    namespaces.(field_lo,field_len); by-value accessors db_namespace_member_count/
    _at. db_query_namespace_type (type.c): NAMESPACE_ITEMS dep → filter pub →
    def_identity per member → scratch → bulk-append → ip_get(IPK_NAMESPACE_TYPE,
    {nsid}); member types lazy (type_of_def(member.def)); fp = fold(name,def) +
    IpIndex.v → membership-firewalled (body-edit stable, pub-toggle flips). Stub
    deleted. DELETED obsolete import_resolution_test/import_cascade_test (pre-D1,
    full-$(SRCS) + arena namespace_type.field_*). Gate: tools/namespace_type_test.
  - pool compaction: compact_aggregate_field_pool / _enum_variant_pool /
    _namespace_field_pool (engine_compact.c) mirror compact_body_scope_pools —
    liveness = the owning def's/namespace's TYPE_OF_DECL/NAMESPACE_TYPE slot is
    QUERY_DONE (collect_paged_slot_ranges for struct/union/enum PagedVec columns;
    a Vec-column walk for namespaces), reusing compact_one_subpool with cell =
    the (lo) column entry @ offset 0. Wired into pools_maybe_compact +
    last_compacted_* trackers + compact_stats[2..4]. Gate: tools/pool_compaction_test
    (churn → compactor fires → survivors correct, ASan clean). Full keep-zone green.
  - DEFERRED to Phase-8 (per review): intern-pool index GC (ip_remove-on-orphan +
    ip_compact + the full IpIndex remap) — near-dormant post-D2.1b (no ip_remove
    callers; ~13 bytes/rename; coupled to the defs free-list / def.idx reuse).
    (The dead `exported` scope field was removed in D2.2c, below.)

D2.2b — meta-flow cleanup [DONE] (post-D2.2 review)
  - type_of_def now reads `meta` from its top_level_entry result (`e.meta`), NOT
    defs.meta. type_of_def deps on top_level_entry (content firewall) but NOT on
    def_identity (which writes defs.meta), so reading defs.meta relied on implicit
    "def_identity ran first" ordering — correct-by-accident. e.meta is correct-by-
    construction: top_level_entry's fp is the decl's green_structural_hash, which
    hashes every non-trivia token by its hash-consed pointer (ptr == kind+text,
    green.c:85), so ANY modifier change (pub<->pvt, +distinct, even as text-
    distinguished IDENTs) flips the hash → flips the fp → type_of_def re-runs with
    fresh meta. Behavior-preserving today (distinct typing deferred to D2.4).
    defs.meta KEPT as the def's resolved-meta column (def_identity writes it; D2.5
    unused.c reads it for the visibility filter) — type_of_def is just decoupled.
  - decl_meta_of FOLDED into decl_classify: the walk now makes ONE classification
    call per decl returning (name, kind, meta) — the after-bind-op modifier scan
    runs inside decl_classify on the same node, no separate second pass (restores
    D2.0b's classify-once consolidation).
  - namespace_type micro-cleanups: pub members appended INLINE to namespace_field_pool
    (no request-arena scratch + deferred bulk-append — nothing else appends to that
    pool during the loop, so the range stays contiguous); success fp is the
    (name,def) content fold ONLY (dropped the constant inline-nominal IpIndex.v).
  - REJECTED after re-analysis: splitting the membership fp / a NAMESPACE_VIS query
    to stop namespace_scopes/def_identity re-running on a pub/distinct toggle. The
    meta-fold must be evaluated every edit to detect a visibility toggle (it backdates
    an AstId-only fp); today it piggybacks the NAMESPACE_ITEMS walk's existing fp
    (zero extra passes), so a NAMESPACE_VIS query would add a 2nd O(items) fold +
    dispatch PER KEYSTROKE to remove a RARE name-layer re-run that already cuts off
    downstream (namespace_scopes' output fp is visibility-independent). Keystrokes ≫
    visibility toggles ⇒ the split increases total cycles. DELIBERATE TRADE: keep meta
    in the NAMESPACE_ITEMS membership fp; the rare namespace_scopes re-run on a
    completed pub/distinct toggle (identical output, cuts off) is the lower-total-
    cycles choice — not a bug.
  Gate: full keep-zone green under ASan (test-classify/-type-of-def/-namespace-type/
    -pool-compaction + name-layer tests); behavior-preserving refactor.

D2.2c — wart cleanup [DONE] (post-review)
  - Removed the dead NamespaceScopes.exported field (always SCOPE_ID_NONE,
    subsumed by NAMESPACE_TYPE). NamespaceScopes is now just {internal}. Dropped
    its lone write (scope.c) + the always-false exported-liveness branch in
    compact_decl_pool (engine_compact.c) + the test assertion (namespace_scopes_test).
    namespace_scopes_read/write memcpy the whole struct, so shrinking it is
    transparent. The COLUMN name `namespaces.exports` is left as-is (historical;
    renaming ripples through ids.c + every read/write site) — noted in db.h.
  - defs.meta: KEPT (decided). Write-only in the keep-zone today (def_identity
    writes it; type_of_def reads meta via top_level_entry as of D2.2b) — pre-
    provisioned for D2.5 unused.c's visibility filter, NOT dead. Commented at the
    column (ORE_DEFS_COLUMNS) + the scope.c write so the next reader knows.
  Gate: full keep-zone green under ASan (incl. compact path: orphan-reclaim +
    pool-compaction; updated namespace_scopes test). Behavior-preserving.

D2.3 — body_scopes [DONE] (structural-only, RA ExprScopes-aligned)
  - NEW src/db/query/body_scopes.c: db_query_body_scopes(fn) builds the fn body's
    scope tree (ScopeRow.parent) + name bindings + node→scope map (FnBody.scope_map),
    PURELY STRUCTURAL — no types. The old sema/body_scopes.c conflated scope-building
    with type inference (ScopedBind.type + eager sema_type_of_expr/check_expr in the
    walk); that's removed. All typing → infer_body (D2.4), keyed off bind_site.
  - SCHEMA: ScopedBind {scope_id, name, IpIndex type} → {scope_id, name, SyntaxNodePtr
    bind_site} (the binding's node — RA BindingId analogue; disambiguates same-scope
    shadowing). No keep-zone consumer read ScopedBind.type. ScopeRow + the embedded
    scope_map + its orphan-reclaim free hook unchanged; compact_body_scope_pools is
    transparent to the element-size change.
  - Dep shape = type_of_def's: top_level_entry(nsid,name) is the SOLE content firewall;
    green root read RAW from files.green_roots (NO file_ast dep → sibling firewall).
    NO fn_signature dep (param names from the param syntax), NO TOP_LEVEL_INDEX ensure,
    NO decl_ast, NO multi-file loop. Because the walk does NO typing it calls no nested
    queries → the old re-entrancy hazard, partial-FnBody publish, and per-push cell
    re-fetch all vanish (atomic build: append rows/binds, fill FnBody once at the end).
  - lookup: db_body_scope_lookup(fn, use, name) ensures via db_query_body_scopes (dep)
    → scope_map hit → walk scope→parent, latest-in-scope wins (shadowing) → returns the
    winning bind's bind_site ({kind=NONE} on miss). Returns the BINDING, not a type.
  - fp = POSITION-INDEPENDENT structural fold: ⊕(scope.parent) ⊕ (bind.scope_id,
    bind.name.idx) in push order — NOT block_node/bind_site byte ranges (trivia-safe,
    same as the D2.1 NodeTypeBuilder fix). Flips on a new/renamed local or new scope;
    STABLE across a pure value edit (x:=a → x:=7) so body_scopes cuts off there while
    infer_body re-runs on its own content dep.
  - Cleanup vs the port: dropped the phantom empty else-scope (old code scope_push'd an
    else scope even with no else branch) — only push then/else scopes that exist.
  Gate: tools/body_scopes_test (structure + bind_site lookup + fp stable-on-value/
    flips-on-rename/sibling-firewalled) + full keep-zone green under ASan. Stub deleted.

D2.4 — body inference [DONE] (type_of_expr/check_expr + infer_body)
  - NEW src/db/query/infer.c: ports sema/{type_of_expr,check_expr,infer_body}.c
    onto the D1–D2.3 layer (those 3 sema files deleted). type_of_expr (synth) +
    check_expr (bidirectional + can_coerce v1 variance + comptime-leaf re-stamp)
    are non-memoized HELPERS (declared in type_layer.h); db_query_infer_body is
    the memoized query.
  - LOCAL TYPES via the node-map (the key rewiring; body_scopes is structural as
    of D2.3). infer owns local types: a let-bind statement (SK_CONST_DECL/VAR_DECL
    in a body) computes its type (annotation via resolve_type_expr, else
    type_of_expr(RHS)) and node_type_builder_push'es it keyed by the DECL node
    (its bind_site); params push their fn_signature type keyed by the Param node
    BEFORE the body walk; if-let unwraps the cond's optional, pushed at the cond
    node. A local ref resolves via db_body_scope_lookup → bind_site → hashmap_get
    on the ACTIVE builder map. No SemaCtx.locals, no second map. (Resolves the
    D2.3 tag-all defer: hover/D3 looks up decl nodes too, so tag-all stays.)
  - FIELD ACCESS rewired off the intern pool (D2.1b/D2.2 made nominals inline):
    struct/enum field/variant → DefId from {struct,enum}_type.zir_node_id →
    db_query_type_of_def(dep) → db_aggregate_field_type / db_enum_variants;
    namespace member → namespace_type.nsid → db_query_namespace_type(dep) →
    db_namespace_member_*. (Old code read ek.struct_type.field_names — gone.)
  - COMPLETED COMMON STATEMENTS beyond the old switch (which only had ~14 expr
    kinds): let-bind, if (synth + bidirectional, incl. if-let), loop, assignment,
    defer/expr-stmt, break/continue. So real fn bodies (let + if + loop + return)
    typecheck end-to-end. (match/switch typing still falls to the "not implemented"
    diag — a documented follow-on; the old code lacked it too.)
  - infer_body deps: top_level_entry (content firewall — preamble resolves the
    lambda from the raw green root, NO file_ast/decl_ast/TOP_LEVEL_INDEX/multi-file
    loop) + fn_signature (declared return, for the return-check) + body_scopes
    (scope structure) + transitively the type_of_def/fn_signature of referenced
    decls. Writes the body node→type map to db.fns.body_node_types (infer_body_write,
    frees prior); fp = the position-independent node-type fold (body type-change
    flips, sibling cuts off). NO re-entrancy hazard (body_scopes structural).
  - type_of_def inferred-bind path wired (type.c): KIND_CONSTANT/VARIABLE with no
    annotation → type_of_expr(RHS) with enclosing_fn=NONE (e.g. x :: 42 →
    comptime_int). infer_body stub deleted.
  - UNKNOWN-NAME DIAGS wired: type position — resolve_type_expr's SK_REF_TYPE/
    SK_PATH_TYPE emit "unknown type 'X'" on a non-zero-name IP_NONE miss; expr
    position — resolve_value_path emits "undefined identifier 'X'". Mismatch diags
    upgraded to "expected %T, got %T".
  - DEFERRED (confirmed): builtins → D3 (SK_BUILTIN_EXPR → IP_NONE; @import is the
    only one + it's cross-namespace); comptime VALUES (IPK_INT_VALUE/FLOAT_VALUE,
    range-check, const_eval) → Phase-6 (D2.4 does comptime TYPES + coercion only).
  Gate: tools/infer_body_test (inferred bind → comptime_int; param + local-let
    refs type via bind_site→node-map; fp flips on body edit, sibling-firewalled) +
    full keep-zone green under ASan.
  - D2.3 deferred items still stand (lookup O(depth×binds); scope_map node-ptr
    hash keys; fresh-HashMap-per-rebuild; repeated ephemeral red-tree builds) —
    none load-bearing; revisit under profiling.

D2.4b — body checker completion [DONE] (switch + nested-lambda + orelse + block-expr)
  - "ONE SWITCH" grammar consolidation: the parser only ever emitted SK_MATCH_EXPR
    for `switch (…)`; SK_SWITCH_STMT/SK_IF_STMT/SK_LOOP_STMT were vestigial (the
    language is expression-only). Renamed SK_MATCH_EXPR → SK_SWITCH_EXPR + the
    MatchExpr wrapper → SwitchExpr (SwitchExpr_scrutinee/_arms); DELETED the three
    dead STMT kinds + their IfStmt/LoopStmt/SwitchStmt wrappers + the orphaned
    nth_block_or_if helper. Touched syntax_kind.h/.c, parse_expr.c, ast_expr.h/.c,
    ast_stmt.h/.c, infer.c. No test referenced the removed kinds.
  - SWITCH typing (infer_switch, NET-NEW): scrutinee type → per-arm patterns checked
    against it via check_expr (reuses the enum-ref/literal paths; `_` parses as a
    LITERAL(SK_UNDERSCORE) → types IP_NONE → coerces; `|`-alternation handled by
    iterating arm node-children, last = body, rest = patterns); arm bodies checked
    against `expected` (bidirectional) or synthesized + unified (synth, unify_arith).
    Basic ENUM exhaustiveness: collect covered variant names (capped 64) → require
    every db_enum_variants entry covered OR a `_` wildcard, else diag. Wired in both
    type_of_expr (synth) + check_expr (bidirectional).
  - NESTED LAMBDA (signature-only): exported build_fn_type (un-static, declared in
    type_layer.h); type_of_expr's SK_LAMBDA_EXPR → build_fn_type → the lambda's fn
    type. The lambda BODY is NOT walked — deferred (body_scopes recurses transparently
    into nested lambdas without isolating their param scope; full nested-body inference
    needs a body_scopes extension).
  - orelse: in SK_BIN_EXPR, `a orelse b` with `a:?T` → T (the unwrapped optional;
    b is the fallback, may be noreturn); non-optional `a` → diag.
  - block-as-expr: SK_BLOCK_EXPR shares the SK_BLOCK_STMT tail-type handling
    (BlockStmt{.syntax=node} bypasses the STMT-only cast since the case validates kind).
  Gate: tools/infer_body_test extended (switch arms unify to i32; ?i32 orelse i32 → i32;
    nested lambda → fn type) + full keep-zone green under ASan (incl. syntax_kind/
    ast_wrappers after the rename).
  - DEFERRED (documented): nested-lambda BODY inference; int-range switch exhaustiveness
    + pattern-binding vars; effect handlers (SK_HANDLE_EXPR) — effects, out of scope;
    loop init/cond/step (LoopExpr exposes only _body).

D2.5 — check driver + unused-decl warnings + dead-column/stub cleanup [DONE]
  - DESIGN (salsa/RA-grounded, web-research 2026-05-29): salsa's early-cutoff is
    VALUE-EQUALITY on the whole memoized output (C::values_equal → the user Eq impl,
    salsa/src/function/backdate.rs) — there is NO output fingerprint. RA's memoized
    InferenceResult stores RESOLUTIONS alongside types (crates/hir-ty/src/infer.rs),
    so a reference change changes the value → dependents re-run for free. ore diverges:
    hand-rolled db_fp_* fingerprints. So a MEMOIZED unused-query over the per-decl fps
    would go STALE on a same-type reference swap (return foo→return bar, both i32:
    infer_body re-runs but its node-type fold/fp is unchanged → backdates → the usage
    query never re-runs). RA's own top-level aggregator semantic_diagnostics()
    (crates/ide-diagnostics/src/lib.rs) is a PLAIN fn over salsa-cached queries, and RA
    does NOT incrementalize cross-declaration reachability (whole-program dead-code →
    rustc/flycheck; its native unused analyses are per-body/local). ⇒ both the driver
    AND the unused pass are PLAIN functions; only the per-decl type queries stay memoized.
  - DRIVER (check.c, db_check_namespace — plain fn, NOT a query): ensure
    namespace_scopes is live (it owns the unused diag unit), then per decl →
    def_identity → type_of_def (all kinds; a fn also runs fn_signature) + infer_body
    (KIND_FUNCTION ONLY — infer_body/fn_signature/body_scopes slot routing is fn-only,
    so a non-fn call trips db_query_begin's "slot kind not wired" assert; the old
    sema/check.c called it unconditionally but was never compiled/run against this
    engine). Type/return/unknown-name errors land on each decl's own slot DiagList;
    consumers call db_check_namespace then db_collect_diags_for_file.
  - UNUSED PASS (check.c, plain — recomputes from the CURRENT dep graph each check, so
    always correct, no fp ⇒ no staleness): the dependency graph IS the reference graph —
    "D references X" ⟺ "type_of_def(X) ∈ D's {type_of_def,fn_signature,infer_body}
    TYPE_OF_DECL deps". Union those (filtered to kind==QUERY_TYPE_OF_DECL) across decls
    → referenced set; flag each decl NOT referenced AND not pub (NamespaceItem.meta &
    META_VIS_MASK == VIS_PUBLIC) AND not `main`. Emit "%S is declared but never used"
    to the NAMESPACE_SCOPES(nsid) diag unit via db_emit_to, db_diags_clear'd + re-emitted
    each check (unit is live + namespace-keyed → db_collect_diags_for_file's liveness
    gate keeps the warnings; driver runs the pass LAST so they're fresh at collect).
  - NEW ENGINE INTROSPECTION (engine.{h,c}, privileged read-only): db_slot_dep_count +
    db_slot_dep_at (→ QueryDepRef{kind,key}) over a slot's recorded deps Vec; mirrors
    db_slot_fingerprint's locate-then-read. Returns 0 / a sentinel for an absent slot,
    so reading FN_SIGNATURE/INFER_BODY deps of a non-fn (unroutable) is safe.
  - DEAD COLUMNS REMOVED (db.h ORE_DEFS_COLUMNS): defs.ref_count (the old impure
    resolve_ref counter, dead since the D1 rewrite) AND defs.meta (the dep-graph design
    reads visibility from NamespaceItem.meta, so defs.meta was product-dead — written by
    def_identity, read by no keep-zone product code). The X-macro auto-drops their
    init/push_zero/free in ids.c; scope.c's def_identity meta-write removed; classify_test
    repointed to read visibility off NamespaceItem.meta (the live source).
  - CLEANUP: deleted the comment-only stubs.c (all query bodies migrated) + the
    ported/superseded sema sources (check, unused, dump, type_resolve, fn_signature,
    type_of_def, body_scopes). src/sema now holds only builtins.{c,h} + sema.h (D3:
    builtins + the cross-namespace/@import rewrite; sema.h kept — compiler/ide/driver
    still include it, all D3 rewrite targets).
  - INTERN-POOL AUDIT (confirm, no code change): the items_tag/items_data two-vector SoA
    is DoD-justified + KEPT — the hot bucket probe reads the 1-byte items_tag
    (tag/tombstone filter) before any items_data/arena touch. ip_compact stays a deferred
    Phase-8 stub (removal tombstones in place; reclamation paired with the defs free-list).
  Gate: tools/check_test (type error surfaces; unused = unreferenced-private with
    pub/main/referenced exempt; incremental ref-edit flips the warning; the same-type
    ref-swap MOVES the warning — the case a fp-memoized usage query would miss) + full
    keep-zone green under ASan.
  - DEFERRED: ip_remove/ip_compact + defs free-list → Phase-8; D3 consumer rewrite
    (sema.h consumers, DeclEntry.node_ptr→.def, cross-namespace @import). Self-reference
    counts as "used" (a recursive-but-unreachable fn isn't flagged) — matches the old
    ref_count behavior; revisit only if real dead-code precision is wanted.

Phase D — remaining
  D3.   Rewrite sema/ide/compiler on top of query wrappers (incl. switching
        DeclEntry.node_ptr→.def callers; the `exported` scope).
  Phase-8. Intern-pool index GC (ip_remove/ip_compact) + defs free-list; the
        remaining Phase-8 GC cluster.
