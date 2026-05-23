# Ore compiler architecture

A 5-minute tour. For details on any subsystem, follow the header-file
links — those are the authoritative reference. This doc explains how
the pieces fit together.

## The compiler in 60 seconds

Ore is a query-driven incremental compiler in the rust-analyzer / Roslyn
lineage. Source bytes flow through a layered pipeline; every layer's
output is memoized in a salsa-style query engine; edits invalidate
only what their dep graph actually touches.

```
LSP client / CLI driver
        │   workspace_did_open / didChange / didEvict / didChangeWatchedFiles
        ▼
[ workspace coordinator ]      ← src/db/workspace/   (SOLE owner of disk I/O)
        │   db_create_source, db_create_file, db_create_namespace,
        │   workspace_admit_virtual, workspace_resolve_import
        ▼
[ db (storage + queries) ]     ← src/db/
        │   sources / files / namespaces / defs (SoA columns)
        │   intern pool (types + values)
        │   query engine (memoized derivations)
        ▼
[ sema ]                       ← src/sema/
        │   type_of_def, type_of_expr, infer_body, body_scopes,
        │   builtins dispatch
        ▼
[ consumers ]                  ← src/consumers/
            driver, lsp, ide
```

## Architectural commitments

These are **load-bearing**. Adding code that violates them is bad.
Each is documented in detail at the cited header.

1. **File = Namespace 1:1:1.** Each `.ore` source is its own file is
   its own namespace. Sibling files do NOT share scope; cross-file
   access requires `@import("./other.ore")`. Matches Zig's
   `Namespace.owner_type` model. See header of
   [src/db/workspace/workspace.h](../src/db/workspace/workspace.h)
   for the full substrate-boundary doc.

2. **Workspace owns I/O.** Disk reads, `realpath` canonicalization,
   source registration are concentrated in
   [src/db/workspace/workspace.c](../src/db/workspace/workspace.c).
   Sema, queries, parser — none of them touch the filesystem. This
   is the chokepoint that lets us drop in a VFS or mock filesystem
   without refactoring downstream.

3. **Lazy disk reads do not bump revisions** (Roslyn / rust-analyzer
   "lazy inputs" model). When `@import("./b.ore")` lazy-loads a file
   that hasn't been registered yet, the admit is a *discovery* of
   pre-existing state, not a *mutation*. No `db_input_changed` call.
   This preserves the salsa purity invariant: queries never mutate
   inputs.

4. **Stable IDs for the lifetime of the process.** `SourceId`,
   `FileId`, `NamespaceId`, `DefId`, `AstId`, `StrId` — once allocated,
   the row exists until the db is freed. Eviction (FS deletion) zeroes
   content but leaves identity intact. Downstream consumers treat IDs
   as monotonic, never-invalidated handles. The hardest part of
   incremental compilers — invariant breakage from ID reuse — is
   sidestepped by construction.

5. **Substrate boundary: source-shaped vs IR-shaped.** Anything
   expressible as source code (disk files, virtual comptime-generated
   source, `@embedFile` content, macro expansions) shares the single
   source/file/namespace table. Anything derived (AST, types, body
   inference, future MIR, future monomorphization, comptime values)
   gets its own per-kind side table. **Don't try to express MIR or
   monomorphization as virtual files** — they're not source-shaped.
   See the substrate-boundary doc at the top of
   [src/db/workspace/workspace.h](../src/db/workspace/workspace.h).

6. **Query engine invariants.** Slots are RUNNING transient,
   never cross request boundaries. Cycles return early via
   `DB_QUERY_GUARD` without caching the cycle result. The full
   invariant list (slot states, revision semantics, request
   boundaries, cacheability, invalidation, diag survival,
   cancellation, body-author rules) is at the top of
   [src/db/query/query.h](../src/db/query/query.h).

## Subsystem map

### `src/db/workspace/` — input coordinator

The single layer that calls db input setters. LSP and driver route
through it; sema calls into it via `workspace_resolve_import` for
`@import` resolution. Owns `realpath` canonicalization. Handles
lazy disk loads, virtual file admit, FS watcher events (via
`workspace_did_change_external` / `workspace_did_evict_source`).
Read [workspace.h](../src/db/workspace/workspace.h) first.

### `src/db/setters/` and `src/db/getters/`

Input boundary and read accessors over the db's SoA columns.
`db_create_source`, `db_create_file`, `db_create_namespace`,
`db_admit_virtual_source`, `db_create_virtual_file`,
`db_set_source_text` on the write side. `db_get_source_*`,
`db_get_file_*`, `db_get_namespace_*` on the read side. No business
logic — these are mechanical column accessors that the higher layers
go through so we can audit the I/O surface in one place.

### `src/db/query/` — the query engine + per-query bodies

The memoization machinery (`db_query_begin`, `db_query_succeed`,
`DB_QUERY_GUARD`, dep tracking, fingerprint-based early cutoff,
cycle detection) lives in `query.c` / `query.h` / `invalidate.c` /
`dispatch.c`. Read [query.h](../src/db/query/query.h)'s header for the
invariants. Per-query bodies (one file each): `ast.c` (parse),
`index.c` (top-level decl index per namespace), `module_exports.c`
(namespace internal scope), `namespace_type.c` (namespace-as-struct-type),
`def_identity.c`, `type_of_def.c`, `fn_signature.c`, `infer_body.c`,
`body_scopes.c`, `resolve_ref.c`, `decl_ast.c`, `node_to_def.c`,
`file_imports.c`. Each query is keyed by a `QueryKind` enum entry +
a u64 key; each has a slot in either a per-row SoA column or a routed
HashMap entry.

### `src/db/intern_pool/` — type + value interning

The single global pool of (type, value) entries. `IpKey` is the
public-facing builder; `IpIndex` is the opaque hash. Types: pointers,
slices, optionals, arrays, fns, structs, enums, effect rows,
namespace types (the file-as-struct-type kind). Values: ints, floats,
reserved constants (nil, true, false). All de-duped by content;
identical inputs collapse to identical `IpIndex`. See
[intern_pool.h](../src/db/intern_pool/intern_pool.h).

### `src/db/diag/` — diagnostic types

`Diag`, `DiagArg`, `DiagSeverity`, `ResolvedSpan` definitions plus
the emit/format/collect/clear function declarations. Diagnostics
live in the central `db.diag_lists` table keyed by `(QueryKind, key)`
— not per-slot. Cleared on recompute + input invalidation. Bodies in
`src/db/setters/diag.c` and `src/db/getters/diag.c`.

### `src/db/storage/` — primitives

Vec, Arena, HashMap, StringPool. Used everywhere. No domain logic.

### `src/db/ids/` — ID types + lifecycle

`SourceId`, `FileId`, `NamespaceId`, `DefId`, `ScopeId`, `AstId`,
`StrId` typedefs + per-type validity helpers. `db_ids_init` /
`db_ids_free` initialize and tear down all the SoA columns via
X-macros. `FILE_ID_VIRTUAL_BIT` and `file_id_make_physical` /
`file_id_make_virtual` live here.

### `src/db/request/` — request lifecycle

`db_request_begin(revision)` pins `effective_revision` for the
duration of a sema run. `db_request_end` unpins, sweeps any RUNNING
slots (Phase 1f defensive cleanup), resets the request arena.
Cancellation, current_revision vs effective_revision distinction.

### `src/lexer/`, `src/parser/`

Front-end. Token stream → AST. The parser owns trivia handling and
the per-file AST arena. Output (`ASTStore`) is stored on the file row;
re-parse on edit, salsa caches per-file via QUERY_FILE_AST.

### `src/sema/` — semantic analysis

`type_of_def`, `type_of_expr`, `infer_body`, `body_scopes`, plus the
builtin dispatch table in [builtins.h](../src/sema/builtins.h) /
`builtins.c`. Sema is a pure consumer of the db's queries — it never
mutates inputs, never reads disk, never holds slot pointers across
nested query calls.

### `src/consumers/`

The two entry points: `driver/` is the CLI (`./ore build foo.ore`);
`lsp/` is the LSP server (JSON-RPC over stdin/stdout via cJSON).
`ide/` has shared helpers (position conversion, references). Both
consumers route through `oredb_*` wrappers in `lsp/db.c` or the
driver-internal in `driver/build.c`, which in turn drive the
workspace coordinator + run sema inside a `db_request_begin/end`
block.

## Where do I look for X?

| You want to… | Read first | Then look at |
|---|---|---|
| Add a new query kind | [query.h](../src/db/query/query.h) header | `dispatch.c`, `invalidate.c`, a similar existing query |
| Add a new builtin (e.g. `@sizeOf`) | [src/sema/builtins.h](../src/sema/builtins.h) | Add a row to the table in `builtins.c` |
| Understand the substrate boundary | [src/db/workspace/workspace.h](../src/db/workspace/workspace.h) header | n/a |
| Understand cycle handling | [src/db/query/query.h](../src/db/query/query.h) header §4–§5 | `query.c:db_query_begin` |
| Understand how `@import` flows | [src/db/workspace/workspace.c](../src/db/workspace/workspace.c) `workspace_resolve_import` | `src/sema/builtins.c` `builtin_import` |
| Understand how field access works (`b.foo`) | [src/sema/type_of_expr.c](../src/sema/type_of_expr.c) `AST_EXPR_FIELD` case | `namespace_type.c` for the struct-type build |
| Understand revision / invalidation | [query.h](../src/db/query/query.h) header §2 + §5 | `request.c`, `db_input_changed` in `query.c` |
| Add an LSP feature | [src/consumers/lsp/server.c](../src/consumers/lsp/server.c) | The `dispatch` function — wire a new method handler |
| Hack on the parser | [src/parser/](../src/parser/) | `parse_decl.c`, `parse_expr.c`, `parser.h` |

## Phases of work landed

The architectural commitments above didn't appear all at once.
History (high-level, for context):

- **Phase 1** — Lazy workspace I/O, file-as-namespace, engine RUNNING
  sweep. Closed the import gap; killed the directory-as-module
  policy; established the workspace-owns-I/O invariant.
- **Phase 2** — Namespace = struct type (Zig-aligned), global rename
  `ModuleId → NamespaceId`, dead-code purge of `IPK_NAMESPACE`,
  `db_module_for_directory`, `module_by_directory`. Unified field
  access in sema (no special `IP_TAG_NAMESPACE` branch).
- **Phase 3** — Comptime-ready substrate: FS watcher + source
  eviction; virtual file admit; builtin dispatch table; substrate-
  boundary doc; query engine invariants doc.

The "comptime drop-in" promise: when comptime work begins, it's pure
additive code — adding aggregate value `IpKey` kinds, a
`db_query_const_eval` body, builtin handler rows. Zero substrate
refactors required.

## What's deferred

Known gaps with conscious trade-offs:

- **VFS abstraction layer**: not built. Workspace calls `slurp_file`
  directly. Tests can't inject mock files; production correctness
  unaffected. Add when test mocking becomes painful.
- **Source text free on eviction**: not done. Eviction marks the row
  evicted + bumps revision but doesn't free the text buffer. A V2
  follow-up adds the free after auditing all `sources.texts` readers
  (today: `db_resolve_span` reads it for diag-line rendering, which
  would UAF on freed text without an evicted-bit gate). Memory churn
  bounded by repo size.
- **Comptime engine**: the interpreter, `db_query_const_eval` body,
  comptime call cache, aggregate value `IpKey` kinds. Substrate is
  ready; the engine is a separate chunk.
- **MIR / codegen**: deferred. When it arrives, MIR will be per-fn
  side tables (`db.fns.mir`), NOT virtual files. The substrate-
  boundary rule in `workspace.h` says this explicitly.
- **`@build` build system**: declared via Ore source. Will be a
  discovery driver atop the workspace API. No new substrate needed.
- **Dynamic LSP watcher registration**: today the LSP uses static
  registration (works for VS Code, Neovim, Helix). Dynamic
  `client/registerCapability` registration is a later refinement.
- **`workspace_did_close` ref-counting**: today a no-op stub. When
  long LSP sessions show memory pressure from accumulated closed
  files, add ref-counted eviction.

## Glossary

- **Source** — raw bytes of one .ore file (or one virtual / comptime-
  generated buffer). `SourceId` row in `db.sources`.
- **File** — the parse unit; one-to-one with a source today (1:1 may
  generalize later, but not in any current plans). `FileId` row in
  `db.files`. Has a virtual bit (`FILE_ID_VIRTUAL_BIT`) for synthesized
  files.
- **Namespace** — the scope formed by a file's top-level declarations.
  One-to-one with a file. `NamespaceId`. The namespace IS a struct
  type at the intern-pool level (`IPK_NAMESPACE_TYPE`) whose fields
  are the file's public decls. Sibling files do NOT share scope.
- **Def** — a top-level declaration (const, var, fn, struct, enum,
  effect, handler, ...). `DefId`. Stable identity per `(NamespaceId,
  AstId)` pair via `db_query_def_identity`.
- **AST** — abstract syntax tree, per-file, built by the parser.
- **AstId** — content-addressed handle for an AST node; reparse-stable.
- **AstNodeId** — index into the file's current parse; NOT reparse-
  stable.
- **Query** — a memoized pure function. Body lives in
  `src/db/query/<name>.c`; slot stored either as a per-row SoA column
  (per-File, per-Namespace, per-Def) or routed via a HashMap (e.g.
  `def_by_identity`).
- **Request** — a salsa "session." Bracketed by `db_request_begin` /
  `db_request_end`. Pins `effective_revision` for consistent reads.
- **Slot** — the memoized state for one (query, key). Stores result
  fingerprint, deps, durability, verified revision.
- **Fingerprint** — a u64 hash of a query's result. Drives early-
  cutoff: if a recompute produces the same fingerprint, downstream
  consumers skip re-running.
- **Durability** — a tier (`DUR_LOW`, `DUR_MEDIUM`, `DUR_HIGH`) per
  input, encoding "how often does this change." Inputs bump
  `dur_last_changed[d]`; verify can skip dep walks for slots whose
  durability is at a tier with no recent change.
- **Cycle** — a query that re-enters its own slot during computation.
  Returned via `QUERY_BEGIN_CYCLE` from `db_query_begin`; the
  `DB_QUERY_GUARD` macro routes this to the body's cycle-sentinel
  return. Cycle results are NOT cached.
- **Eviction** — marking a source as deleted (typically from an FS
  watcher event). Stable IDs preserved; the row's `evicted` bit is
  set and downstream iteration filters skip it.
- **Substrate** — the shared source/file/namespace table that holds
  both disk-backed and virtual sources. The "boundary" between
  source-shaped and derived-IR-shaped data.
