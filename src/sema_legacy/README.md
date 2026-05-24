# `sema_legacy/` — reference only, not built

This directory holds the pre-rewrite sema implementation. It is **NOT
compiled into the `ore` binary** — `src/sema_legacy/` is not listed in
the Makefile's `CORE_DIRS`, so `make all` ignores it entirely.

The active implementation lives in [`../sema/`](../sema/). Cross-cutting
behavior (lazy import, file-as-namespace, intrinsic-error diagnostics,
discard warnings, etc.) is in `../sema/` only.

## Why we keep it on disk

There are several patterns in here that the new sema hasn't ported yet
and that future work will need to reference. Specifically:

- **Constant folding** ([`typechecker/coerce.c`](typechecker/coerce.c))
  — the comptime-int / comptime-float range checks, integer overflow
  detection, and float-to-int conversion rules. The new sema's
  `can_coerce` ([`../sema/check_expr.c`](../sema/check_expr.c)) is a
  structural-only port; the value-level checks land with the comptime
  engine.

- **Effect handler scoping** ([`name_resolution/`](name_resolution/))
  — how `with` clauses bring effect operations into lexical scope, and
  how `stderr`, `alloc`, etc. resolve through that scope. Surfaced by
  the failing `examples/test.ore` baseline; the proper fix uses this
  as reference.

- **Body inference of comptime expressions** ([`body/`](body/) +
  [`comptime/`](comptime/)) — the partial evaluator + comptime call
  caching. Substrate for the comptime work that's still ahead.

- **Workspace inputs / source tracking** ([`workspace/`](workspace/))
  — pre-Phase-1c source registration patterns. Less relevant now that
  `src/db/workspace/` owns this layer; mostly historical at this point.

## Working policy

- **Don't wire it into the build.** Adding `src/sema_legacy/` to
  `CORE_DIRS` will produce a flood of symbol collisions and duplicate
  StrId names — these files were written against a pre-rewrite db.h
  that no longer exists.

- **Port patterns by hand, don't extern.** When future work needs one
  of these patterns (e.g., the constant-folding tables in `coerce.c`),
  copy the relevant code into `../sema/` and adapt it to the current
  db API. Don't try to `#include "../sema_legacy/typechecker/coerce.c"`
  or similar.

- **Delete pieces once they're truly obsolete.** When a directory in
  here no longer holds anything not already in `../sema/`, delete it
  from git. The git history preserves the reference indefinitely;
  on-disk presence is for the porting-in-progress window.

- **The grep risk is the main cost.** Pattern: "where do we handle
  fn coercion?" → `grep -rn coerce src/` → hits both `../sema/` and
  here. Filter mentally, or `grep -rn coerce src/ --exclude-dir=sema_legacy`.
