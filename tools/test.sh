#!/bin/sh
#
# Smoke + integration tests for `make test`.
#
# Three layers:
#   1. C foundation smoke tests (arena, hashmap) built with ASan.
#      Compiled via TEST_CC (defaults to `clang`) rather than the
#      main-build CC (`zig cc`), because zig cc's bundled
#      compiler-rt is missing macOS ASan symbols
#      (`___asan_version_mismatch_check_v8`, B22). Real clang ships
#      libclang_rt.asan_{osx_dynamic,x86_64,aarch64} for both
#      darwin and linux, so the same TEST_CC works on every
#      supported platform.
#
#   2. Per-fixture exit-code verification for examples/tests/. Naming
#      convention: `*_errors.ore` must exit non-zero, every other
#      fixture must exit 0. This catches "fixture that used to fail
#      now silently passes" and vice versa — neither
#      `make test-invalidation` nor `make test-determinism` covers
#      that property today.
#
#   3. (Implicit) Cross-validates the build itself by re-invoking
#      `make all` if `./ore` is stale.
#
# Pre-PR-3-Layer-1 this script grew to ~1400 lines of inline `.ore`
# test material that referenced moved source files and pruned
# example fixtures (B1). The diagnostic-substring tests it carried
# are subsumed by examples/tests/*_errors.ore which the production
# sweep here exercises by exit code; the specific stderr-match
# coverage is captured via the fixture comments documenting what
# each error spelling should be.

set -u

PROJECT_ROOT=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
cd "$PROJECT_ROOT" || exit 2

ORE="$PROJECT_ROOT/ore"
FIXTURES_DIR="$PROJECT_ROOT/examples/tests"

# CC, TEST_CC, and TEST_CFLAGS may be set by the Makefile; provide
# sane defaults so `sh tools/test.sh` works standalone.
#
# CC      — used implicitly when re-invoking `make` if ore is stale.
# TEST_CC — sanitizer-clean C compiler for the smoke tests. Defaults
#           to `clang` because zig cc + ASan is broken on macOS (B22).
: "${CC:=zig cc}"
: "${TEST_CC:=clang}"
: "${TEST_CFLAGS:=-std=c17 -Wall -Isrc -g -fsanitize=address}"

TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/ore-tests.XXXXXX") || exit 2
trap 'rm -rf "$TMP_DIR"' EXIT HUP INT TERM

PASS=0
FAIL=0

pass() { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
fail() { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }

# ---------- 1. C foundation smoke tests ----------

build_and_run_smoke() {
    name=$1
    src=$2
    extra_srcs=$3
    out="$TMP_DIR/$name"
    log="$TMP_DIR/$name.log"

    if ! $TEST_CC $TEST_CFLAGS "$src" $extra_srcs -o "$out" >"$log" 2>&1; then
        fail "$name (build failed)"
        sed 's/^/    /' "$log"
        return
    fi
    if "$out" >"$log" 2>&1; then
        pass "$name"
    else
        fail "$name (run failed)"
        sed 's/^/    /' "$log"
    fi
}

printf 'C foundation smoke tests (TEST_CC=%s, ASan)\n' "$TEST_CC"
build_and_run_smoke "arena_test"   "tools/arena_test.c"   "src/common/arena.c"
build_and_run_smoke "hashmap_test" "tools/hashmap_test.c" "src/common/hashmap.c src/common/arena.c"

# ---------- 2. Per-fixture exit-code verification ----------

if [ ! -x "$ORE" ]; then
    printf '\nbuilding ore (stale)\n'
    if ! make -s all; then
        printf 'FAIL: ore build\n'
        exit 1
    fi
fi

printf '\nFixture exit codes (examples/tests/)\n'
for fixture in "$FIXTURES_DIR"/*.ore; do
    [ -f "$fixture" ] || continue
    name=$(basename "$fixture" .ore)
    "$ORE" build --quiet "$fixture" >/dev/null 2>&1
    rc=$?
    case "$name" in
        *_errors)
            if [ "$rc" -ne 0 ]; then
                pass "$name (expected fail, got rc=$rc)"
            else
                fail "$name (expected fail, got rc=0)"
            fi
            ;;
        *)
            if [ "$rc" -eq 0 ]; then
                pass "$name"
            else
                fail "$name (expected pass, got rc=$rc)"
            fi
            ;;
    esac
done

# ---------- summary ----------

printf '\n%d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
