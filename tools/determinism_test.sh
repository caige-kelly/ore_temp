#!/bin/sh
#
# Determinism regression test.
#
# Runs `./ore --dump-resolve --dump-const-eval --dump-tyck` twice on
# every test fixture in examples/tests/ and bytewise-compares the two
# runs. Any drift means something in the pipeline is non-deterministic
# (HashMap iteration order leaking into output, uninitialized memory
# being read, etc.) and is a bug we want to catch immediately.

set -u

PROJECT_ROOT=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
ORE="$PROJECT_ROOT/ore"
FIXTURES_DIR="$PROJECT_ROOT/examples/tests"

if [ ! -x "$ORE" ]; then
    echo "fatal: $ORE not built. Run 'make all' first." >&2
    exit 2
fi
if [ ! -d "$FIXTURES_DIR" ]; then
    echo "fatal: no fixtures at $FIXTURES_DIR" >&2
    exit 2
fi

PASS=0
FAIL=0
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/ore-determinism.XXXXXX") || exit 2
trap 'rm -rf "$TMP_DIR"' EXIT HUP INT TERM

for fixture in "$FIXTURES_DIR"/*.ore; do
    [ -f "$fixture" ] || continue
    name=$(basename "$fixture" .ore)
    out_a="$TMP_DIR/$name.a"
    out_b="$TMP_DIR/$name.b"

    # Combine stdout (dumps) and stderr (diagnostics) so the test
    # also catches non-determinism in error ordering.
    "$ORE" --dump-resolve --dump-const-eval --dump-tyck --no-color \
        "$fixture" >"$out_a" 2>&1 || true
    "$ORE" --dump-resolve --dump-const-eval --dump-tyck --no-color \
        "$fixture" >"$out_b" 2>&1 || true

    if cmp -s "$out_a" "$out_b"; then
        PASS=$((PASS + 1))
        printf "  ok   %s\n" "$name"
    else
        FAIL=$((FAIL + 1))
        printf "  FAIL %s — output differs across runs\n" "$name"
        diff -u "$out_a" "$out_b" | head -20
    fi
done

printf "\n%d pass, %d fail\n" "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
