#!/usr/bin/env bash
# Three lint gates protecting the query / IDE layer's purity:
#
# Gate A — Untracked column reads:
#   Fail if src/db/query/ or src/ide/ contains any raw access pattern
#   that should go through the capability layer. Catches both direct
#   column reads (`s->defs.kinds`) AND address-of escapes
#   (`&s->defs` — splits across lines, function-arg passes, casts,
#   macro args).
#
# Gate B — Input mutator calls from queries:
#   Queries and IDE handlers must never invoke an input mutator
#   (db_create_*, db_set_source_*, db_readmit_*, db_namespace_remove_*,
#   workspace_did_*). Inputs are the boundary where the DB absorbs
#   new state from the outside world; semantic passes must be pure
#   demand-driven readers. A mutation from inside a query body
#   bypasses the engine's transaction model and corrupts cache
#   consistency.
#
# Gate C — FILE_RAW diag anchors from cached queries:
#   `diag_anchor_make` / `diag_anchor_of_node` produce DIAG_ANCHOR_FILE_RAW
#   (byte-frozen) anchors. When emitted from a Phase-3.1-cacheable query
#   (TYPE_OF_DECL / FN_SIGNATURE / INFER_BODY / BODY_SCOPES), they DRIFT
#   on byte-shifting sibling edits — the bug Phase-3.1 follow-up fixed
#   by wiring the DeclAstIdMap into span_of. New emits in src/db/query/
#   must either use span_of (which prefers the structural anchor) OR
#   carry a `// LINT_FILE_RAW_OK` marker with a one-line justification.
#   See docs/diag-anchor-audit.md.
#
# Allowlist via the trailing `// LINT_UNTRACKED_OK` comment, scoped to
# the line, justified in code review. capability.c is the read-gate
# (allowed raw reads). engine*.c owns the slot tables. workspace.c
# and inputs/ ARE the mutator boundary — they're never linted here.

set -e

# ERE syntax — `grep -E` below treats `( | )` as operators directly.
# No backslashes inside the groups, or grep will hunt for literal `\(`.
FORBIDDEN_DIRECT='\bs->(defs|fns|namespaces|files|scopes)\.\w'
FORBIDDEN_ALIAS='&\s*s->(defs|fns|namespaces|files|scopes)\b'
# Input-boundary mutators: things that admit new state from the outside
# world (filesystem, LSP edits, workspace setup). Producer-internal ID
# allocation (db_create_def, db_create_scope) is NOT in this list —
# those are intrinsic to DEF_IDENTITY / NAMESPACE_SCOPES compute and
# emerge naturally from semantic queries observing the source.
# Note: regex requires `(` IMMEDIATELY after the function name — no
# whitespace allowed. This excludes comment mentions like
# `// db_create_file (via db_input_set)`.
FORBIDDEN_MUTATORS='\b(db_create_(file|file_lazy|virtual_file|source|namespace)|db_set_source_(text|durability)|db_readmit_source|db_namespace_remove_file|workspace_did_(open|change|close|change_external|evict_source))\('

fail=0

hits_reads=$(grep -rnE "($FORBIDDEN_DIRECT|$FORBIDDEN_ALIAS)" \
         src/db/query/ src/ide/ \
         --include='*.c' \
         --exclude='capability.c' \
         --exclude='engine*.c' | \
       grep -v '// LINT_UNTRACKED_OK' || true)

if [ -n "$hits_reads" ]; then
  echo "lint: untracked db reads found (use db_read_* or add // LINT_UNTRACKED_OK):"
  echo "$hits_reads"
  fail=1
fi

hits_mutators=$(grep -rnE "$FORBIDDEN_MUTATORS" \
         src/db/query/ src/ide/ \
         --include='*.c' | \
       grep -v '// LINT_UNTRACKED_OK' || true)

if [ -n "$hits_mutators" ]; then
  echo "lint: input mutator call from query/ide layer (mutations belong in src/db/inputs/ or src/db/workspace/):"
  echo "$hits_mutators"
  fail=1
fi

# Gate C — FILE_RAW diag-anchor emit sites in cached-query paths.
# diag_anchor_make / diag_anchor_of_node always produce DIAG_ANCHOR_FILE_RAW,
# which is byte-frozen and drifts under sibling-edit reparses when the
# owning query caches. Cached-query callers should use a SemaCtx-aware
# helper (span_of in type.c / infer.c — these consult the decl wrapper
# AstIdMap and pick the structural anchor when possible). New emit sites
# must use that path OR carry a per-line `// LINT_FILE_RAW_OK` marker
# with a justification (parse.c's pre-parse errors, check.c's driver
# pass that resets its bundle on every emit, coerce.c's coarse "byte 0"
# row-unify cycle diag).
FORBIDDEN_FILE_RAW='\b(diag_anchor_make|diag_anchor_of_node)\('
hits_file_raw=$(grep -rnE "$FORBIDDEN_FILE_RAW" \
         src/db/query/ \
         --include='*.c' | \
       grep -v '// LINT_FILE_RAW_OK' || true)

if [ -n "$hits_file_raw" ]; then
  echo "lint: FILE_RAW diag-anchor emit found (use span_of() or add // LINT_FILE_RAW_OK — see docs/diag-anchor-audit.md):"
  echo "$hits_file_raw"
  fail=1
fi

if [ "$fail" = "1" ]; then
  exit 1
fi
echo "lint: ok"
