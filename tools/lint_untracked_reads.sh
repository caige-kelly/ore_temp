#!/usr/bin/env bash
# Two lint gates protecting the query / IDE layer's purity:
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

if [ "$fail" = "1" ]; then
  exit 1
fi
echo "lint: ok"
