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

Phase C — First consumer
  C1. Parse-layer helpers (top-level decl walker, structural fingerprint)
  C2. db_query_file_ast real implementation (push-stamps DECL_AST + TOP_LEVEL_ENTRY)
  C3. db_query_decl_ast + db_query_file_imports real implementations

Phase D — Downstream layers (the multi-week work)
  D1. scope.c (scope/name layer queries)
  D2. type.c (type queries)
  D3. Rewrite sema/ide/compiler on top of query wrappers
