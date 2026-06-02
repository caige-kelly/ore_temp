# `src/db/` — How the database fits together

This document maps the directory's pieces, their dependency order, and
where overlap or drift could surface. Read this before adding a new
table column, a new query kind, or a new layer.

If something here drifts from the code, the code wins — file an item to
fix the doc.

---

## 1. The big picture

`src/db/` is a salsa-style memoized derivation engine over a single
arena-backed in-memory store (`struct db`). Five concentric layers,
strict dependency direction (inner can be called by outer; never the
reverse):

```
┌───────────────────────────────────────────────────────────────────┐
│  CONSUMERS         compiler/  ide/  consumers/  tools/            │
│  (outside src/db/) Open requests, drive queries, render diags.    │
└───────────┬──────────────────────────────┬───────────┬────────────┘
            │                              │           │
            ▼                              ▼           ▼
   ┌────────────────┐         ┌────────────────────┐  ┌────────────┐
   │  workspace/    │         │  query/            │  │  getters/  │
   │  LSP entry +   │         │  Memoized derived  │  │  Read-only │
   │  path canon.   │         │  computation +     │  │  accessors │
   │                │         │  capability layer  │  │            │
   └────────┬───────┘         └────────┬───────────┘  └─────┬──────┘
            │                          │                    │
            ▼                          ▼                    ▼
   ┌────────────────┐         ┌────────────────────────────────────┐
   │  inputs/       │         │  diag/  +  ids/  +  intern_pool/   │
   │  Mutate state  │         │  Cross-layer support utilities     │
   │  (db_create_*) │         │                                    │
   └────────┬───────┘         └────────────────────────────────────┘
            │                          │
            ▼                          ▼
   ┌───────────────────────────────────────────────────────────────┐
   │  db.h  +  db.c  +  names.inc                                  │
   │  The struct db itself: SoA columns, hashmaps, query slots,    │
   │  per-query result columns, diag bundles, primitives, names.   │
   └───────────────────────────────────────────────────────────────┘
```

The hard rules:
- **No layer above** modifies state outside its boundary. Only
  `inputs/` and `workspace/` write to `struct db` from the consumer
  side.
- **Queries are pure functions of their declared inputs**. They read
  via `getters/` or the capability wrapper, and write ONLY into their
  own per-query result column at compute time.
- **The capability layer (`query/capability.h`) is the single gate**
  for cross-query reads. Bypassing it is lint-enforced
  (`tools/lint_untracked_reads.sh`).

---

## 2. Folder map

### `src/db/` root

| File | Purpose |
|---|---|
| `db.h` | The DB's struct definition + all public types. Includes diag/, ids/, intern_pool/, query/engine.h. The "kitchen sink" header — every other file in `src/db/` includes it. |
| `db.c` | `db_init` / `db_free` lifecycle. Allocates per-query slot vectors, seeds primitives, opens the first request. |
| `names.inc` | Pre-interned builtin identifier table (e.g. `LEN`, `i32`, `usize`). Consumed by `db_init` to stamp `s->names`. |

### `ids/` — Typed identity handles

Pure types module — no `struct db` access, no allocation.

| File | Purpose |
|---|---|
| `ids.h` | `FileId`, `SourceId`, `NamespaceId`, `DefId`, `ScopeId`, `StrId`, `AstId`. Each is `{ uint32_t idx }` wrapped for compile-time type safety. Slot 0 is the NONE sentinel. |
| `ids.c` | The producer-internal allocators called by query computes: `db_create_def`, `db_create_scope`. Each `db_create_*` here grows the SoA columns for that entity (`s->defs.*`, `s->scopes.*`) by one zero row in lockstep. |

**Naming note**: `db_create_def` / `db_create_scope` live here (`ids.c`),
NOT in `inputs/`. They are producer-internal — emerge from semantic
queries observing the source, not from external mutations. The lint
allows them inside `query/` because of this.

### `inputs/` — Input mutator boundary

The Salsa-input-setter equivalent. Mutates `struct db` from the
**outside-world** boundary (LSP edit, FS watcher, workspace setup).

| File | Purpose |
|---|---|
| `source.c` | `db_create_source`, `db_set_source_text`, `db_set_source_durability`. Each `db_set_source_*` bumps the `QUERY_SOURCE_TEXT` input slot's fingerprint via `db_input_set`. |
| `file.c` | `db_create_file`, `db_create_file_lazy`, `db_create_virtual_file`, `db_readmit_source`, `db_namespace_remove_file`. Each grows the per-file SoA columns and folds the file into the namespace's `QUERY_FILE_SET` fingerprint via incremental `db_fp_combine`. |
| `module.c` | `db_create_namespace`. Stamps a fresh namespace's columns + initializes its `QUERY_FILE_SET` input slot. |

**Hard invariant** (runtime-asserted): no `inputs/` mutator may be
called inside an open query frame. The engine asserts via
`db_input_changed`. This is the structural enforcement of "you cannot
alter the past while calculating the future."

### `workspace/` — Path + LSP transaction layer

| File | Purpose |
|---|---|
| `path.{h,c}` | `canonicalize_path` — realpath wrapper used by all LSP entrypoints. |
| `workspace.{h,c}` | `workspace_did_open`, `workspace_did_change`, `workspace_did_close`, `workspace_did_change_external`, `workspace_did_evict_source`. Each maps an LSP-event URI → SourceId, calls the matching `inputs/` mutator. |

Conceptually `workspace/` is the LSP-facing dispatcher over `inputs/`.
Together they form the input boundary; consumers in `src/consumers/lsp/`
call only `workspace_did_*`.

### `getters/` — Read-only accessors

Pure read functions. No allocation, no fingerprint bumps, no query
firing. They serve "I have an X, give me its Y" lookups against the
SoA columns. Used by drivers, IDE handlers, and tests.

| File | Purpose |
|---|---|
| `source.c` | `db_get_source_text`, `db_get_source_path`, `db_get_source_durability`, `db_get_source_evicted`, etc. |
| `file.c` | `db_get_file_source`, `db_get_file_namespace`, `db_lookup_file_by_source`. |
| `module.c` | `db_get_namespace_files`. |
| `position.c` | Byte-offset ↔ line/col lookups (via `LINE_INDEX` query results). |
| `type.c` | Type rendering / formatting (uses intern pool + names table). |
| `diag.c` | `db_collect_diags_for_file` — the diag-gather pass that walks every per-query DiagBundle, gates by `db_slot_is_live`. |

**Naming overlap to be aware of**: there is a `getters/type.c` AND a
`query/type.c`. Different concerns:
- `getters/type.c` — type RENDERING (formatting an `IpIndex` to a
  human string). No query frame needed.
- `query/type.c` — the `TYPE_OF_DECL` / `FN_SIGNATURE` query
  implementations. Memoized compute.

### `intern_pool/` — Content-addressed types + values

| File | Purpose |
|---|---|
| `intern_pool.{h,c}` | `IpIndex` ↔ `IpKey`. One `IpIndex` covers types, comptime values, and effect rows. `ip_get(key)` is structural-dedup; same key always returns the same IpIndex. Lookups are content-addressed and never go stale — no salsa dep needed. |
| `ip_primitives.def` | The boot table: `i32 → IpIndex 1`, `bool → IpIndex 2`, etc. Stamped by `db_init`. |

### `diag/` — Diagnostic infrastructure

| File | Purpose |
|---|---|
| `diag.h` | The public diag types: `Diag`, `DiagAnchor`, `DiagSink`, `DiagBundle`, `DiagSeverity`, `DiagTag`. Defines the per-query DiagBundle contract. |
| `sink.c` | `db_emit` — reads the active query frame's installed sink and appends. Asserts a frame is active + sink installed. The single emit gate. |
| `ast_id.{h,c}` | `BodyAstIdMap` — preorder-index → SyntaxNode mapping per fn body, used by diag anchors to survive incremental reparses without storing byte ranges. |

### `query/` — Memoized derivation engine + queries

The bulk of the compiler's incremental logic. 24 files split four
ways:

**Engine implementation** (the salsa core):

| File | Purpose |
|---|---|
| `engine.h` | Public engine API: `db_request_begin/end`, `db_query_*_begin/succeed`, `DB_QUERY_GUARD` macro, `ORE_QUERY_KINDS` X-macro listing all 17 query kinds. |
| `engine_internal.h` | Privileged primitives (slot state writes, dep tracking, slot reclamation). Includers must `#define ORE_ENGINE_PRIVATE`. Compile-error otherwise. |
| `engine.c` | `db_query_begin` / `db_query_succeed` / dep recording / slot lifecycle. |
| `engine_verify.c` | Early-cut path: walk a slot's deps, compare recorded fingerprints to current values, decide recompute-vs-cached. |
| `engine_dispatch.c` | The recompute dispatch table — maps `(QueryKind, key)` → the right compute function. |
| `engine_compact.c` | Pool compaction (aggregate-fields, enum-variants, namespace-members, scope-decl pools). Run when garbage accumulates. |
| `engine_fingerprint.c` | `db_fp_*` helpers (`db_fp_u64`, `db_fp_combine`, etc.). |
| `result_columns.h` | Per-query result-column accessors (`fn_signature_read/write`, `infer_body_read/write`, etc.). Privileged like `engine_internal.h`. |

**The capability layer** (the dep-tracking gate — see `migration.md`):

| File | Purpose |
|---|---|
| `capability.h` | `db_read_*` (tracked: assert active frame, record dep) and `db_get_*_untracked` (driver / producer-self) wrappers. The SINGLE place outside `engine*.c` allowed to access raw SoA columns. |
| `capability.c` | Implementation. Excluded from the lint by name. |

**Query implementations** (one file or section per query kind):

| File | Owns queries |
|---|---|
| `parse.c` | `FILE_AST`, `LINE_INDEX`, `FILE_IMPORTS`, `NAMESPACE_ITEMS`, `TOP_LEVEL_ENTRY`. |
| `scope.c` | `DEF_IDENTITY`, `NAMESPACE_SCOPES`, `RESOLVE_REF`. |
| `type.c` | `TYPE_OF_DECL`, `FN_SIGNATURE`, `NAMESPACE_TYPE`. |
| `infer.c` | `INFER_BODY`. Large — handles type-of-expr, effect rows, all binop / call / ctrl-flow type rules, the Phase B terminator pass. |
| `body_scopes.c` | `BODY_SCOPES`. Body-local lexical scopes (per-fn). |
| `node_type.c` | `db_query_node_type` — IDE-facing router that resolves a `SyntaxNode` → its type via the right producing query. NOT a memoized query — a plain function composing memoized lookups. |
| `check.c` | `db_check_namespace` — non-memoized driver. Types every decl in a namespace + emits unused-decl warnings. Owns the `CHECK` input slot's diag bundle. |
| `const_eval.c` | `eval_inner` — non-memoized comptime evaluator. Folds const expressions on demand from type-of-expr / coerce. |
| `coerce.{h,c}` | Bidirectional type-coerce rules + `unify_arith`. Called by `infer.c` / `type.c`. Not a query — a pure rule library. |
| `builtins.{h,c}` | `@sizeOf`, `@ptrCast`, `@intCast`, `@import`, etc. Called from `infer.c`. |
| `type_layer.h` | `node_types_range_lookup` — small helper shared by `node_type.c` and the per-fn type queries. |

**Documentation**:

| File | Purpose |
|---|---|
| `migration.md` | Long-form changelog + Phase B+1 capability-layer doc. |

---

## 3. The data store (`db.h`)

`struct db` is the SoA root. Key sub-structures:

```
struct db {
    // ─── Lifecycle / control ────────────────────────────────────
    Arena            arena, request_arena;   // long-lived + per-request
    StringPool       strings;                // StrId backing
    atomic uint64    rev_control;            // revision counter

    // ─── Content-addressed ──────────────────────────────────────
    InternPool       intern;                 // IpKey → IpIndex
    DbNames          names;                  // pre-interned builtins
    NodeCache       *node_cache;             // structural sharing (green)

    // ─── Engine state ───────────────────────────────────────────
    Vec query_stack;                         // active frames
    QueryStats query_stats[QUERY_KIND_COUNT];

    // ─── Result tables (per-entity SoA columns) ────────────────
    FilesTable       files;       // per-FileId   ─── parsed by inputs/file.c
    SourcesTable     sources;     // per-SourceId  ─── inputs/source.c
    NamespacesTable  namespaces;  // per-NsId     ─── inputs/module.c
    DefsTable        defs;        // per-DefId    ─── ids/ids.c (allocator)
    FnsTable         fns;         // per-fn DefId ─── grows in lockstep with defs
    ScopesTable      scopes;      // per-ScopeId  ─── ids/ids.c (allocator)
    /* + structs / unions / enums / effects / handlers / variables / constants */

    // ─── Cross-table indexes ───────────────────────────────────
    HashMap source_by_path, file_by_source, def_by_identity, ...
};
```

Each sub-table is itself an SoA — multiple parallel `Vec<T>` columns
keyed by the entity's `.idx`. The X-macro `ORE_FILES_COLUMNS`,
`ORE_DEFS_COLUMNS`, etc. ensure every column grows by one row in
lockstep on a `db_create_*` call.

Per-query result columns are part of the entity-sub-table they
correspond to (e.g., `fns.signature_result`, `fns.body_node_types`,
`fns.body`). Per-query DiagBundles live alongside the result
(`fns.fn_body_diags`, `defs.type_of_decl_diags`). The slot's
fingerprint (in `query_slot_*` columns) gates whether the result is
current.

---

## 4. The 17 query kinds (from `ORE_QUERY_KINDS`)

Ordered by layer, leaf-first. Input-class slots (set, never computed)
are marked `↧`. Derived-class are `▶`.

### Input layer
- `↧ SOURCE_TEXT` — per-source content fingerprint. Set by
  `db_set_source_text`.
- `↧ FILE_SET` — per-namespace membership fingerprint. Folded by
  `db_create_file` / `db_namespace_remove_file`.
- `↧ CHECK` — driver-owned diag-bundle slot. Set live by
  `db_check_namespace` on every pass.

### Parse layer (`parse.c`)
- `▶ FILE_AST` — green tree for a file. Depends on `SOURCE_TEXT`.
- `▶ LINE_INDEX` — per-file line-start byte offsets.
- `▶ FILE_IMPORTS` — per-file `@import` references.

### Scope / name layer (`parse.c` + `scope.c`)
- `▶ NAMESPACE_ITEMS` (`parse.c`) — per-namespace top-level items list.
  Depends on `FILE_SET` + `FILE_AST` of each member file.
- `▶ TOP_LEVEL_ENTRY` (`parse.c`) — per-(namespace, name) entry. The
  content firewall: structural-hash of the decl wrapper survives sibling
  edits.
- `▶ NAMESPACE_SCOPES` (`scope.c`) — the namespace's internal scope.
  Each binding = (name, DefId). Allocates ScopeIds via
  `db_create_scope`.
- `▶ DEF_IDENTITY` (`scope.c`) — `(NamespaceId, AstId) → DefId`.
  Allocates DefIds via `db_create_def`. Stamps `defs.names`,
  `defs.parent_modules`, `defs.identity_keys`.
- `▶ RESOLVE_REF` (`scope.c`) — per-`(scope, name)` resolution. Walks
  parent scope chain.

### Type layer (`type.c` + `infer.c` + `body_scopes.c`)
- `▶ TYPE_OF_DECL` (`type.c`) — a decl's overall type. For structs /
  unions / enums: builds the nominal type. For typed binds: resolves
  the annotation. For fns: delegates to `FN_SIGNATURE`.
- `▶ FN_SIGNATURE` (`type.c`) — params + return + effect row. Writes
  `fns.signature_result`. Side-effect-emits per-sig-position node
  types into `fns.signature_diags` via the diag sink.
- `▶ INFER_BODY` (`infer.c`) — fn-body type inference. Big.
- `▶ BODY_SCOPES` (`body_scopes.c`) — lexical scope tree within a fn
  body. Allocates a per-fn `FnBody { scope_map, scope_rows, binds }`.
- `▶ NAMESPACE_TYPE` (`type.c`) — `IPK_NAMESPACE_TYPE` for a
  namespace's exported member list. Side-effect-writes the
  `namespaces.field_lo` / `field_len` columns indexing
  `namespace_field_pool` (a SAFE co-product write — same nsid entity
  key on both sides).

---

## 5. Concrete data flow — LSP `didChange`

End-to-end trace of "the user typed a character":

```
1. consumers/lsp/server.c::handle_did_change
     receives the JSON-RPC notification
     extracts (uri, version, new_text)

2. consumers/lsp/db.c::oredb_did_change
     stale-packet version check
     calls →

3. db/workspace/workspace.c::workspace_did_change
     canonicalize_path(uri) → path
     SourceId src = db_lookup_source_by_path(...)
     db_readmit_source(src)            ─── inputs/file.c
     db_set_source_text(src, text)     ─── inputs/source.c
       └─ bumps QUERY_SOURCE_TEXT slot fingerprint via db_input_set

4. consumers/lsp/db.c::oredb_typecheck
     calls compile_file(src) →

5. compiler/compile.c::compile_file
     db_request_begin(rev)
     db_query_file_ast(fid)            ─── parse.c, recompute under new text
     db_request_end()

     db_request_begin(rev)
     db_check_namespace(nsid)          ─── query/check.c, driver
       └─ for each decl in NAMESPACE_ITEMS:
           db_query_type_of_def(def)   ─── query/type.c
              └─ for fn: db_query_fn_signature, db_query_infer_body, db_query_body_scopes
       └─ emit_unused_warnings()       ─── walks NAMESPACE_ITEMS, checks deps
     db_query_line_index(fid)          ─── parse.c, for diag offset→line/col
     db_request_end()

     db_collect_diags_for_file(fid)    ─── getters/diag.c
       └─ walks per-query DiagBundles, gates by db_slot_is_live
       └─ returns Vec<Diag> to the LSP server

6. consumers/lsp/server.c::publish_diagnostics
     formats Diag → LSP JSON
     sends textDocument/publishDiagnostics
```

Every cross-query read inside steps 5's queries goes through
`query/capability.h` (`db_read_*`) so the dep is recorded. Every diag
emit goes through `db_emit` (`diag/sink.c`) so it lands in the active
frame's bundle.

---

## 6. Folder isolation policy (lint-enforced)

`tools/lint_untracked_reads.sh` runs as part of `make test`. Two gates:

**Gate A** — raw SoA-column access:
```
forbidden:  s->{defs,fns,namespaces,files,scopes}.X
forbidden:  &s->{defs,fns,namespaces,files,scopes}
scope:      src/db/query/*.c, src/ide/*.c
excluded:   src/db/query/capability.c, src/db/query/engine*.c
escape:     // LINT_UNTRACKED_OK: <reason>
```

**Gate B** — input mutator calls:
```
forbidden:  db_create_(file|file_lazy|virtual_file|source|namespace)(
forbidden:  db_set_source_(text|durability)(
forbidden:  db_readmit_source(
forbidden:  db_namespace_remove_file(
forbidden:  workspace_did_(open|change|close|change_external|evict_source)(
scope:      src/db/query/*.c, src/ide/*.c
rationale:  mutations belong in src/db/inputs/ or src/db/workspace/
```

Note that producer-internal ID allocators (`db_create_def`,
`db_create_scope`) live in `ids.c` and are NOT in Gate B. They're
intrinsic to `DEF_IDENTITY` / `NAMESPACE_SCOPES` compute.

---

## 7. Audit notes — overlaps + design risks

Things to keep an eye on:

### Producer-side raw writes inside `query/*`

Several query computes still use raw pointer casts to write into their
own slot's parallel columns:

- `query/type.c` — `namespaces.field_lo / field_len` writes inside
  `NAMESPACE_TYPE` compute.
- `query/scope.c` — `defs.names / parent_modules / identity_keys`
  stamps inside `DEF_IDENTITY`; `scopes.parents / meta /
  owning_modules / decl_lo / decl_len` writes inside
  `NAMESPACE_SCOPES`.
- `query/body_scopes.c` + `query/infer.c` — `fns.body_ast_id_maps[row]`
  writes inside `INFER_BODY` / `BODY_SCOPES`.

All are **safe co-product writes** (same entity key both sides) and
annotated `// LINT_UNTRACKED_OK: producer write`. But each escape-
hatch annotation is convention-only — a future copy-paste could turn
a safe co-product into a toxic side-channel.

Filed as **follow-up #10**: consolidate into typed setter API in
`capability.c` (`db_write_namespace_type_outputs`,
`db_write_fn_body_ast_id_map`, etc.). The query files stop using raw
pointer casts entirely.

### Member-list getters that skip dep tracking

`db.h` exposes `db_namespace_member_count` / `db_namespace_member_at`
as RAW getters reading `s->namespaces.field_lo / field_len`. Current
callers in query frames manually anchor with
`db_query_namespace_type(s, ns)` first (e.g.
[infer.c:1629](query/infer.c#L1629)). The API doesn't FORCE the dep.

Filed as **follow-up #10a**: add `db_read_namespace_member_*` in
`capability.h` that fires the producing query internally.

### Two-name overlap

`getters/type.c` (type formatting) and `query/type.c`
(`TYPE_OF_DECL` query) share a filename. Different concerns. Worth a
mental note when navigating; potential rename target if it ever
confuses anyone.

### Engine split across 6 files

`engine.c`, `engine_compact.c`, `engine_dispatch.c`,
`engine_fingerprint.c`, `engine_verify.c`, `engine_internal.h` — split
by concern, not by accident. The split is structural (compact + verify
+ dispatch can move independently). Don't merge them; the file names
ARE the contract.

### `node_type.c` is not a query

Despite living in `query/`, `db_query_node_type` is a **plain
function** that composes memoized queries. The `db_query_` prefix is
historical. Not a problem in practice but worth knowing — searching
for "all queries" via `db_query_*` will surface this and the user
should know it's NOT memoized.

Same applies to `db_check_namespace` (it's a driver, not a query).

### `check.c` writes `CHECK` slot diags without a producing-query frame

`db_check_namespace` is a driver — it owns the CHECK slot's diag
bundle and resets it on each invocation. The CHECK slot is INPUT-class
(set, never computed). This is fine but means `check.c` reaches deeper
into engine internals than other layer files. Pattern is intentional
(driver = sole owner) but worth re-checking if the engine's input-
slot semantics ever change.

### LSP cascade root cause — still uncertain

Per `follow-ups.md #9`, the user observed an LSP cascade-on-edit that
cleared on restart. The capability migration (#12) hardened the
dep-tracking discipline structurally but did NOT reproduce the bug in
the keep-zone keystroke test. The cascade may live in
`consumers/lsp/server.c`'s publish-diagnostics layer rather than the
query path. Worth a focused LSP-trace investigation next time the
symptom surfaces.

---

## 8. Adding new things — quick checklists

### Adding a column to an existing entity (e.g. `defs.foo`)

1. Add to the X-macro list (`ORE_DEFS_COLUMNS` in `db.h`).
2. The X-macro driver in `db_create_def` (`ids/ids.c`) grows it in
   lockstep automatically.
3. Add a `db_read_def_foo(ctx, d)` wrapper in `capability.h/c` if
   queries need to read it cross-boundary.
4. If a producer needs to write it: do so inside the producing
   query's compute. Annotate `// LINT_UNTRACKED_OK: producer write`
   (or, ideally, route through a typed setter in `capability.c` —
   pending follow-up #10).

### Adding a new query kind

1. Add to `ORE_QUERY_KINDS` X-macro in `engine.h`. Tag as INPUT or
   DERIVED.
2. Add result column on the entity sub-table (e.g.
   `fns.your_query_result`).
3. Add `_diags` column if your query emits diagnostics.
4. Add `read_X` / `write_X` accessors in `result_columns.h`.
5. Wire dispatch in `engine_dispatch.c`.
6. Implement the compute in the appropriate layer file
   (`parse.c` / `scope.c` / `type.c` / `infer.c` / `body_scopes.c` /
   a new file).
7. Add `db_read_X(ctx, key)` in `capability.h/c` for tracked
   cross-query reads.

### Adding a new input mutator

1. Implement in `inputs/<entity>.c` or `workspace/`.
2. Stamp identity columns + grow SoA columns via X-macro driver.
3. Bump the right input slot's fingerprint via `db_input_set` if
   queries need precision (per-entity) — OR bump the durability tier
   via `db_input_changed` if it's a coarse "everything at this tier
   may have changed" event.
4. Assert no open query frame (`db_input_changed` does this).
5. Add the function name to `FORBIDDEN_MUTATORS` in
   `tools/lint_untracked_reads.sh` so callers in `query/` or `ide/`
   get blocked.

---

## 9. Where to read next

- `query/migration.md` — long-form changelog including the full
  capability-layer rules.
- `engine.h` top comment — the pure-query model in detail.
- `engine_internal.h` top comment — the privileged-includer list +
  the engine's contract with layer code.
- `capability.h` top comment — when to use `db_read_*` vs
  `db_get_*_untracked` vs `LINT_UNTRACKED_OK`.
- `/home/user/.claude/plans/follow-ups.md` — deferred work (#1–#11),
  including #10/#10a producer-write consolidation and the LSP cascade
  investigation.
