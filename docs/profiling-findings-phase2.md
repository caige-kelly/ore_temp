# Phase 2 findings — LSP latency baseline

## TL;DR

The compiler hot path on `allocator.ore` (~600 lines, 11 fn bodies) takes
**~2.8ms per edit-iter median, steady state.** That is NOT the user's
~500ms perceived delay — most of that delay must be elsewhere
(JSON serialization / LSP transport / editor render).

But profiling DID surface a real architectural bug:

**`INFER_BODY` gets 0% cache hits on body-internal comment-toggle.** Every
function body recomputes every iter even though only ONE body's text
changed. The granularity claim of the salsa engine is broken for this case.

## Measurements

All measurements with `./ore-lsp-workload-release` (`-O2 -g -DNDEBUG`),
nix env, x86_64 Linux. Default workload: `examples/allocator/allocator.ore`.

### Per-iter wall time

| Scenario | Median µs | Cache delta | Compute delta | RSS Δ over 50 iters |
|---|---|---|---|---|
| `noop-edit` (same text reapplied) | ~30 | ~0 | ~0 | ~0 KB |
| `edit-replace` (add real decl) | 2850 | 980 | 106 | +1.7 MB |
| `comment-toggle` (toggle line in body) | 2850 | 1027 | 65 | +1.7 MB |

### Per-kind cache-hit breakdown (comment-toggle, 51 checks)

| Query | Begin | Compute | Hit % | Verdict |
|---|---|---|---|---|
| FILE_AST | 7027 | 52 | 99.3% | ✓ |
| NAMESPACE_ITEMS | 5471 | 52 | 99.0% | ✓ |
| NAMESPACE_SCOPES | 7328 | 2 | 100.0% | ✓ |
| TOP_LEVEL_ENTRY | 5152 | 1025 | 80.1% | OK (top-level decl table edits cascade) |
| DEF_IDENTITY | 3978 | **26** | 99.3% | ✓ (after switching to body-line) |
| RESOLVE_REF | 5687 | **30** | 99.5% | ✓ |
| TYPE_OF_DECL | 6964 | 474 | 93.2% | OK |
| FN_SIGNATURE | 2313 | **561** | 75.7% | ❌ **bug: 11/iter when only 1 body changed** |
| INFER_BODY | 561 | **561** | **0.0%** | ❌ **bug: 11/iter — every body recomputes** |
| BODY_SCOPES | 10240 | 561 | 94.5% | ❌ (mirror of INFER_BODY) |

`noop-edit` gets INFER_BODY 98% hit rate (the hash fast-path skips the
revision bump), so salsa machinery WORKS when nothing actually changes.
The bug is that **comment-toggle invalidates every body even when only one
body's source text moved**.

## Top hot functions (perf record, 200 iters)

```
9.41% hashmap_put            (called from body_ast_id_map_walk, deep recursion)
7.35% malloc
4.64% cfree
4.48% hashmap_get
4.28% wrap_child             (green-tree builder)
3.43% lex_next
3.03% token_child
2.46% syntax_node_child_or_token
2.46% syntax_node_release    (refcount)
2.42% syntax_node_ptr_new
2.18% green_token_compute_hash
2.18% layout_stream          (lexer layout)
1.78% green_structural_hash_rec
1.74% db_query_line_index
1.41% body_ast_id_map_walk   (self)
```

Categorized:
- **~30%** parse + lexer + green-tree construction (`lex_next`, `wrap_child`, `layout_stream`, `green_*`, `node_*`, `syntax_*`)
- **~14%** HashMap ops (NodeTypeBuilder + body_ast_id_map)
- **~12%** malloc / cfree
- **~5%** structural hashing
- **~5%** green-tree refcount management
- rest: query dispatch, sema arms

## Root cause of the granularity bug (hypothesis)

`INFER_BODY` is keyed on DefId (stable across edits). Its input fingerprint
must include something that flips on ANY edit anywhere in the file, not
just changes to the body's own structural hash. Candidates:

1. The body's `SyntaxNodePtr` is resolved against the file's green root.
   If the body's BYTE OFFSET shifted (because earlier lines in the file
   changed length), the resolved SyntaxNode could be flagged as "moved" —
   even though its content didn't change.
2. INFER_BODY may include the file's parse-output fingerprint directly
   instead of just the body's subtree fingerprint.
3. Body dependencies may include a per-file or per-namespace fingerprint
   (e.g., the file's `green_root_hash` or `top_level_index_fp`) that flips
   on any edit — when it should only depend on the symbols actually
   referenced from the body.

Verification path (Phase 3): instrument `INFER_BODY`'s slot to log which
DEP CHANGED to force recompute on each iter. The dep that flips on every
comment-toggle but NOT on noop-edit is the smoking gun.

## RSS / memory growth

50-iter steady-state:
- `noop-edit`: +0 KB (hash fast-path skips work)
- `edit-replace`: +1.7 MB (~33 KB/iter)
- `comment-toggle`: +1.7 MB (~33 KB/iter)

200-iter sample of `edit-replace`: RSS grows from ~5 MB to ~9 MB
(+4 MB / 200 iters = ~20 KB/iter sustained). The growers are:
- `body_scope_rows` pool: ~145 new entries/iter, never shrinks
- `body_scope_binds` pool: ~96 new entries/iter
- `decl_pool`: ~20 new entries/iter
- Compaction fires once after the first iter (`n_compactions=1`) but
  the pools keep growing — the 2× growth-threshold isn't hit again.

This is consistent with the existing design: pools grow until
`count > 2*last_compacted_count`, then mark-and-copy compaction reclaims
the dead ranges. For a 1-hour LSP session with 1000+ edits this would
sawtooth at a manageable level; no unbounded growth observed.

**IP pool**: `intern_count` stays at 62 throughout — no growth on
comment-toggle. IP compaction is NOT in the critical path.

## Phase 3.1 RESULTS — INFER_BODY granularity restored

The fix landed: 4 sites (`infer.c:3030`, `body_scopes.c:551`, `type.c:1255`, `type.c:1354`) had a tracked `db_read_file_ast(ctx, e.file)` call right after a `db_query_top_level_entry(ctx, nsid, name)` dep. The TOP_LEVEL_ENTRY dep is the proper body-stable content firewall; the FILE_AST dep was redundant AND fatal (its whole-file hash flipped on any edit, killing salsa granularity).

Mechanically: replaced each call with `db_read_file_ast_untracked(ctx, e.file)` (new helper in `capability.c`, mirrors the eviction-gated pattern in `getters/position.c:67`). No new query kind, no fingerprint changes — just dropped the over-broad dep.

### Before / After (comment-toggle 50 iters, body-internal line)

| Query | BEFORE compute | AFTER compute | Δ |
|---|---|---|---|
| TYPE_OF_DECL | 474 | 124 | ↓74% |
| FN_SIGNATURE | 561 | 211 | ↓62% |
| **INFER_BODY** | **561 (0% cache)** | **263 (53% cache)** | ↓53% |
| **BODY_SCOPES** | **561 (95% cache)** | **11 (99.8% cache)** | ↓98% |
| TOP_LEVEL_ENTRY | 1025 | 1025 | unchanged (expected — its own behavior is independent) |

Per-iter median wall_us: **2850 → 1350 µs** (53% reduction).

Pool growth flat: `body_scope_rows`, `body_scope_binds`, `decl_pool` no longer grow per iter (bodies don't re-push since they cache). Was: +145 rows/iter. Now: 0 rows/iter steady-state.

### Granularity validated with 2-fn fixture

A clean 2-fn fixture (no transitive deps between bodies) gets the perfect outcome: **1 INFER_BODY compute per iter** (just the edited body). The 5/iter remaining on allocator.ore reflects real transitive deps (allocator's bodies cross-call each other), not a bug. That's correct salsa behavior.

### Regression-locked

A new assertion in [tools/infer_body_test.c](tools/infer_body_test.c) — section (8) — uses `db_query_stats` to check the COMPUTE counter: after editing g's body, INFER_BODY(f) compute count delta MUST be 0. This would have caught the regression instantly; the existing #3c only checked OUTPUT fp stability.

## Conclusion — what to fix in Phase 3

The 500ms user-perceived delay is NOT primarily compiler-side (compiler
takes ~3ms/iter). But the architectural bug — INFER_BODY recomputes
every body when only one body's text changed — IS a real salsa violation
and IS worth fixing.

Phase 3 priorities (data-driven):

1. **TOP PRIORITY — Fix INFER_BODY / FN_SIGNATURE / BODY_SCOPES
   over-invalidation** ([Suspect 3 from the plan, confirmed]). Audit
   `engine_fingerprint.c` and the body's input fingerprint computation.
   Expected payoff: ~250µs/iter saved on multi-body files, BUT bigger
   payoff is correctness/architecture — salsa is supposed to give per-body
   granularity.

2. **Investigate the LSP-side 500ms gap**. Add timestamp logging to the
   `did_change` → `publish_diagnostics` path in
   [server.c](src/consumers/lsp/server.c). The compiler is ~3ms; the
   client-perceived delay is ~500ms. The 497ms must be in: JSON
   serialization, cJSON object construction, stdout write, or LSP client
   processing. Measure before fixing.

3. **Memoize `emit_unused_warnings`** ([check.c:60](src/db/query/check.c#L60))
   as a follow-up. It's not the dominant cost on a 600-line file (didn't
   show up in perf top-30 with significance), but the `calloc(ndefs, 1)`
   per call grows with `ndefs` over a long session — worth bounding.

## Out of scope (de-prioritized by data)

- **Compaction debounce**: per-iter `compact_ns_delta = 0` in steady
  state. Compaction is not on the hot path. Don't tune.
- **IP compaction**: `intern_count` doesn't grow on edits. No bloat
  signal. Don't pursue.
- **ID recycling**: `s->defs.names` grows ~20/iter on edit-replace, but
  this is bounded by orphan-reclaim. No catastrophic growth observed.
