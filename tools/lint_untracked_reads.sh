#!/usr/bin/env bash
# Fail if src/db/query/ or src/ide/ contains any raw access pattern
# that should go through the capability layer. Catches both direct
# column reads (`s->defs.kinds`) AND address-of escapes
# (`&s->defs` — splits across lines, function-arg passes, casts,
# macro args). Allowlist via the trailing `// LINT_UNTRACKED_OK`
# comment, scoped to the line, justified in code review.
#
# capability.c IS the gate — it's allowed to read raw columns. Engine
# internals (engine*.c) own the slot tables themselves and are
# excluded by name.

set -e

# ERE syntax — `grep -E` below treats `( | )` as operators directly.
# No backslashes inside the groups, or grep will hunt for literal `\(`.
FORBIDDEN_DIRECT='\bs->(defs|fns|namespaces|files|scopes)\.\w'
FORBIDDEN_ALIAS='&\s*s->(defs|fns|namespaces|files|scopes)\b'

hits=$(grep -rnE "($FORBIDDEN_DIRECT|$FORBIDDEN_ALIAS)" \
         src/db/query/ src/ide/ \
         --include='*.c' \
         --exclude='capability.c' \
         --exclude='engine*.c' | \
       grep -v '// LINT_UNTRACKED_OK' || true)

if [ -n "$hits" ]; then
  echo "lint: untracked db reads found (use db_read_* or add // LINT_UNTRACKED_OK):"
  echo "$hits"
  exit 1
fi
echo "lint: ok"
