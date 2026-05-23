# Profiling the Ore compiler / LSP

Standardized workflow for measuring performance, memory, and cache
behavior under realistic LSP-style workloads.

## The tool

`tools/lsp_workload.c` is a single binary that drives the compiler
through five scenarios. Each scenario emits per-iteration metrics
(RSS, query compute/cache deltas, table sizes, wall time) plus a
per-`QueryKind` stats dump at the end.

Build:

```bash
make ore-lsp-workload
```

The Makefile target always builds it with `-DORE_DEBUG_QUERIES=1`,
which is required for the query-stats counters.

## Quick start

By default every scenario operates on `examples/allocator/allocator.ore`
(522 lines, ~98 top-level decls — realistic LSP-sized file). Override
with `--file PATH`.

Sanity sweep across all six scenarios with 50 iterations each:

```bash
make profile-workload
```

Or run one scenario at a time:

```bash
./ore-lsp-workload edit-replace --iters 1000
./ore-lsp-workload noop-edit    --iters 1000
./ore-lsp-workload evict-churn  --iters 200
./ore-lsp-workload cross-file   --iters 100 --file path/to/other.ore
```

CSV output (for diffing / plotting):

```bash
./ore-lsp-workload edit-replace --iters 1000 --csv > edit.csv
```

## Scenarios

| Scenario | What it does | What it catches |
|---|---|---|
| `steady-typecheck` | Open file once; re-typecheck N times | Cache hit rate after warmup. After iter 1 every iter should show `compute_delta = 0`. |
| `edit-replace` | Alternate V1 ↔ V2 (V2 = V1 + a real decl appended) N times | Memory stability under realistic semantic edits. RSS should plateau. `compute_delta` per iter ≈ number of top-level decls (top_level_index fingerprint cascades). |
| `noop-edit` | Apply the SAME text N times | The `db_set_source_text` hash fast-path. `compute_delta = 0` for iter ≥ 1; no salsa machinery should fire when text is byte-identical to current. |
| `evict-churn` | Open + evict N distinct files | The V1 eviction leak (per-file arena + source text not freed). RSS growth ≈ N × per-file-cost — see "Concerning signals" below for the actual measured per-iter overhead. |
| `cross-file` | `a.ore` @imports the real file; edit imported N times (V2 = V1 + real pub decl appended) | Cross-file incremental invalidation. Per-iter `compute_delta` should stay small (only b's queries + a's decls referencing changed parts of b). |
| `lazy-load` | One importer with N `@import` targets of the real file | Lazy resolution + namespace_type build cost. Single typecheck pass; should be linear in N. |

## Reading the output

Each row is one iteration. Default (human) format:

```
[   3 edit-replace      ] rss=  1968k  Ccomp=   380 (Δ   94)  Ccache=  1039 (Δ  284)  srcs=  2  intern=    42  +1899us
```

CSV columns:

```
iter, scenario, rss_kb,
compute_total, compute_delta,
cached_total,  cached_delta,
sources_count, intern_count, wall_us
```

Notes:

- `Ccomp` / `Ccache` are **cumulative** counts across all `QueryKind`s.
  `(ΔN)` shows the per-iteration delta — usually what you want.
- `wall_us` is per-iteration wall clock (from start of iter to capture
  point), NOT cumulative.
- `rss_kb` is process **peak** (via `getrusage`'s `ru_maxrss`). Peak
  grows monotonically with workload, which is what we want for spotting
  leaks.
- `sources_count` is the number of registered source rows. For
  `evict-churn` it grows monotonically (stable-IDs invariant). For
  `edit-replace` / `noop-edit` it stays at 2 (the single workload file
  + the row-0 sentinel).
- `intern_count` is the intern pool entry count. Should plateau on
  steady workloads. Grows by ~2 per file in `evict-churn` (per-file
  types interned, never reclaimed — confirmed monotonic by design).

At the end of each scenario, `db_dump_query_stats` prints to **stderr**
a per-`QueryKind` table:

```
kind                      begin   cached  compute  cycle  error  untracked
----                      -----   ------  -------  -----  -----  ---------
file_ast                    52       50        2      0      0          0
type_of_decl              1240     1140      100      0      0          0
infer_body                  87       80        7      0      0          0
namespace_type              10        9        1      0      0          0
...
```

The `cached / begin` ratio per row is the cache hit rate for that
query kind. Above ~0.9 after warmup is healthy. Below ~0.5 means
something is re-running more than it should.

## Healthy signals (what to look for)

Per scenario, here are the expected ranges:

All measurements below are on the default workload
(`examples/allocator/allocator.ore`, 522 lines, ~98 top-level decls)
and run on an Apple Silicon dev machine. Numbers will scale; the
*shape* of the curves is what matters.

### `steady-typecheck`
- Iter 0: cold typecheck. `compute_delta ≈ N_top_level_decls` (98 here).
  Wall time ≈ 600µs–1ms.
- Iter 1+: `compute_delta = 0`. `cached_delta` constant (~57 — once
  per decl per re-typecheck). Wall time drops to single-µs range.
- RSS stable after iter 1.
- No `cycle` counts. No `untracked` counts.

### `edit-replace`
- Iter 0: cold typecheck. `compute_delta ≈ 98`.
- Iter 1+: `compute_delta ≈ N_decls` per iter — adding/removing a
  top-level decl shifts the `top_level_index` fingerprint, which
  cascades. This is *expected* on real semantic edits.
- `cached_delta ≈ 3 × N_decls` per iter (verify walks for
  unchanged decls' deps).
- RSS plateaus.
- `sources_count` stays at 2 (workload file + row-0 sentinel).

### `noop-edit` (the hash fast-path test)
- Iter 0: cold typecheck.
- Iter 1+: **`compute_delta = 0`** — `db_set_source_text`'s hash
  fast-path returns false (no revision bump, no slot staling).
  `cached_delta` constant (the sema_check_module's per-iter walk
  cost — pure cache hits).
- Per-iter wall: ~100µs (dominated by the per-call `realpath` syscall
  + FNV-1a hash of the source bytes — these can't be avoided without
  a smarter LSP layer).
- RSS strictly flat.
- **If `compute_delta > 0` on iter ≥ 1, the hash fast-path is broken.**

### `evict-churn`
- Per-iter `compute_delta`: roughly constant (one full cold typecheck
  per evicted file).
- **RSS grows ~37KB per iter on the default workload** (allocator.ore,
  522 lines, ~98 top-level decls), down from a pre-Phase-8 baseline of
  ~175KB per iter. Phase 8's `workspace_did_evict_source` now frees the
  per-file arena + source text buffer; the residual ~37KB is the
  inherent SoA-row overhead of stable IDs (new rows in sources / files
  / namespaces / per-row slot columns plus ~2 intern-pool entries for
  the file's unique types). Stable IDs are a load-bearing architectural
  commitment; their cost is the floor of evict-churn.
- `sources_count` grows by 1 per iter (stable-IDs invariant).
- `intern_count` grows by ~2 per iter.
- **Phase 8 cleanup safety**: 7 reader sites (db_resolve_span,
  db_get_file_ast, db_get_node_span, db_byte_offset_at,
  db_node_at_offset, db_get_file_ast_id_map, db_get_def_for_node) gate
  on `db_get_source_evicted` so post-free reads bail before
  dereferencing freed pointers. The full evict-churn scenario passes
  cleanly under `-fsanitize=address`.

### `cross-file`
- Iter 0: cold + lazy-loads imported file (allocator.ore). High
  compute.
- Iter 1+: editing the imported file invalidates its queries + the
  importer's queries that reference changed parts. `compute_delta`
  per iter should be small (~5-6) — only `b.ore`'s parse + a few
  cascaded queries.
- If `compute_delta` grows linearly with iter, invalidation is
  over-broad — dig in.

### `lazy-load`
- Single iter; wall time scales linearly with the number of imports
  (~540µs per imported file on this hardware).
- All N imported files registered as sources.
- One `namespace_type` query per import.

## Concerning signals

Things to dig into if you see them:

| Signal | What it means | Where to look |
|---|---|---|
| `cached / begin < 0.5` after warmup | Queries that should be cached are recomputing | Check the query body's deps; missing dep declaration? |
| `recompute_due_to_untracked > 0` | A slot has `has_untracked_read = true` and always re-runs | Search for `has_untracked_read = true` callers |
| `cycle > 0` on non-cyclic code | False-positive cycle detection | Check the recently-added query's CYCLE return path in `DB_QUERY_GUARD` |
| RSS grows on `edit-replace` after warmup | Memory leak on edit path | Run under Instruments → Allocations |
| RSS grows on `evict-churn` substantially faster than the ~37KB/iter Phase 8 floor | Eviction free is regressing — a new per-file column may need adding to the cleanup loop in `workspace_did_evict_source`, or a reader is holding a stale pointer past the evicted-bit gate | Run under Instruments → Allocations / Leaks |
| `cross-file` per-iter compute grows | Over-broad invalidation | Check dep recording in the query bodies that recompute |
| Determinism mismatch (`make test-determinism` fails after a perf-related change) | Nondeterminism (HashMap iteration order, uninit memory) | Run under ASan |

## Attaching Instruments (macOS)

The `--attach-pause` flag makes the binary print its PID then wait
for Enter. Use this to attach Instruments before the workload starts.

1. **Build with debug info** (the Makefile target already includes `-g`):

   ```bash
   make ore-lsp-workload
   ```

2. **Launch with pause**:

   ```bash
   ./ore-lsp-workload edit-replace --iters 1000 --attach-pause
   ```

   You'll see:

   ```
   PID = 91234
   Attach Instruments now, then press Enter to start.
   ```

3. **Open Instruments.app** (Xcode → Open Developer Tool → Instruments,
   or just open `/Applications/Xcode.app/Contents/Applications/
   Instruments.app`).

4. **Pick a template**:
   - **Allocations** — for memory leaks and growth patterns.
   - **Time Profiler** — for CPU hot spots.
   - **Leaks** — for *unreachable* allocations (heaptrack equivalent).
     Note: our V1 eviction leaks (text buffers) will show up here; they
     are intentional. Filter mentally.
   - **System Trace** — for syscall / context switch / page fault detail.

5. **Attach to process**:
   - In Instruments, with the template selected, click the dropdown
     near the top-left (default: "Choose a target") → **All Processes**
     → search for the PID printed by the workload → select.
   - Alternatively: File → Recording Options → choose the PID under
     "Attach to Process".

6. **Press Record** (red circle, top-left) in Instruments.

7. **Return to the terminal and press Enter** to start the workload.

8. **Wait for completion** (or click Stop in Instruments mid-run if you
   only want a portion).

### What to look for in each template

**Allocations**:
- Track "Persistent Bytes" over time. Should be flat after warmup for
  `edit-replace` and `steady-typecheck`. For `evict-churn` should grow
  linearly.
- Sort by "Persistent Bytes" descending. The biggest sources should be
  the source-text buffers, the per-file arenas, the intern pool, and
  the diag arenas — in roughly that order.
- "Heaviest Stack Trace" tells you which call paths allocated the most.

**Time Profiler**:
- Look for any function consuming > 5% of self-time. Likely
  candidates that are healthy:
  `db_query_begin`, `db_verify`, `hashmap_get`, `sema_type_of_expr`,
  `ast_subtree_fingerprint`. If something unexpected dominates (a Vec
  copy, a sequence of small mallocs), that's a hot spot worth fixing.
- "Heaviest Stack Trace" again gives you the call chain.

**Leaks**:
- Filters for memory that's unreachable at the snapshot time. Our
  intentional leaks (eviction text buffers, accumulated IDs) will
  show up. Look for surprises.

## Comparison runs (perf-regression sanity)

To compare before/after a change:

```bash
# Before
git stash
./ore-lsp-workload all --iters 100 --csv > before.csv

# After
git stash pop
make ore-lsp-workload
./ore-lsp-workload all --iters 100 --csv > after.csv

# Diff
diff before.csv after.csv
```

For more nuanced comparisons (per-QueryKind deltas), capture the
stderr stats dump and diff that:

```bash
./ore-lsp-workload all --iters 100 2> before.stats >/dev/null
# ... your change ...
./ore-lsp-workload all --iters 100 2> after.stats >/dev/null
diff before.stats after.stats
```

## What this tool does NOT cover

- **Multi-threaded** behavior — the compiler is single-threaded today;
  parallel workloads are out of scope.
- **Large workspaces** — the harness uses small synthetic files. For
  real-world large-repo profiling, point Instruments at the LSP server
  while you open a real codebase in your editor.
- **LSP protocol overhead** — JSON-RPC encode/decode, stdio. Profile
  the LSP binary (`ore lsp`) directly via Instruments for those costs.
- **Codegen** — doesn't exist yet. When MIR/codegen lands, add a
  scenario here.
