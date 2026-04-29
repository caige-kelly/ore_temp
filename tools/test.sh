#!/bin/sh

set -u

PROJECT_ROOT=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
ORE="$PROJECT_ROOT/ore"
CC=${CC:-clang}
TEST_CFLAGS=${TEST_CFLAGS:-"-std=c17 -Wall -Isrc -fsanitize=address -g"}
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/ore-tests.XXXXXX") || exit 1
ARENA_TEST="$TMP_DIR/arena_test"
HASHMAP_TEST="$TMP_DIR/hashmap_test"

PASS_COUNT=0
FAIL_COUNT=0
TEST_COUNT=0

cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT HUP INT TERM

print_file() {
    file_label=$1
    file_path=$2

    printf '  %s:\n' "$file_label"
    if [ -s "$file_path" ]; then
        sed 's/^/    /' "$file_path"
    else
        printf '    <empty>\n'
    fi
}

pass() {
    test_name=$1
    PASS_COUNT=$((PASS_COUNT + 1))
    printf 'PASS %s\n' "$test_name"
}

fail() {
    test_name=$1
    FAIL_COUNT=$((FAIL_COUNT + 1))
    printf 'FAIL %s\n' "$test_name"
}

run_success() {
    test_name=$1
    shift
    TEST_COUNT=$((TEST_COUNT + 1))
    stdout_file="$TMP_DIR/stdout.$TEST_COUNT"
    stderr_file="$TMP_DIR/stderr.$TEST_COUNT"

    if "$@" >"$stdout_file" 2>"$stderr_file"; then
        pass "$test_name"
        return
    fi

    status=$?
    fail "$test_name"
    printf '  command:'
    printf ' %s' "$@"
    printf '\n'
    printf '  expected: exit 0\n'
    printf '  actual: exit %d\n' "$status"
    print_file stdout "$stdout_file"
    print_file stderr "$stderr_file"
}

run_failure_contains() {
    test_name=$1
    expected=$2
    shift 2
    TEST_COUNT=$((TEST_COUNT + 1))
    stdout_file="$TMP_DIR/stdout.$TEST_COUNT"
    stderr_file="$TMP_DIR/stderr.$TEST_COUNT"

    if "$@" >"$stdout_file" 2>"$stderr_file"; then
        fail "$test_name"
        printf '  command:'
        printf ' %s' "$@"
        printf '\n'
        printf '  expected: nonzero exit and stderr containing: %s\n' "$expected"
        printf '  actual: exit 0\n'
        print_file stdout "$stdout_file"
        print_file stderr "$stderr_file"
        return
    fi

    status=$?
    if grep -Fq "$expected" "$stderr_file"; then
        pass "$test_name"
        return
    fi

    fail "$test_name"
    printf '  command:'
    printf ' %s' "$@"
    printf '\n'
    printf '  expected: nonzero exit and stderr containing: %s\n' "$expected"
    printf '  actual: exit %d\n' "$status"
    print_file stdout "$stdout_file"
    print_file stderr "$stderr_file"
}

cd "$PROJECT_ROOT" || exit 1

printf 'Building ore...\n'
if ! make -s ore; then
    printf 'FAIL build\n'
    exit 1
fi

printf 'Building arena smoke test...\n'
if ! $CC $TEST_CFLAGS tools/arena_test.c src/common/arena.c -o "$ARENA_TEST"; then
    printf 'FAIL arena smoke test build\n'
    exit 1
fi

printf 'Building hashmap smoke test...\n'
if ! $CC $TEST_CFLAGS tools/hashmap_test.c src/common/hashmap.c src/common/arena.c -o "$HASHMAP_TEST"; then
    printf 'FAIL hashmap smoke test build\n'
    exit 1
fi
printf '\n'

run_success "arena chunk growth and marks work" \
    "$ARENA_TEST"
run_success "hashmap heap and arena modes work" \
    "$HASHMAP_TEST"

run_success "import_simple succeeds" \
    "$ORE" --quiet examples/import_simple.ore
run_success "effect_scope_valid succeeds" \
    "$ORE" --quiet examples/effect_scope_valid.ore
run_success "sema_skeleton succeeds" \
    "$ORE" --quiet examples/sema_skeleton.ore
run_success "dump-resolve import_simple succeeds" \
    "$ORE" --dump-resolve --quiet examples/import_simple.ore
run_success "dump-sema sema_skeleton succeeds" \
    "$ORE" --dump-sema --quiet examples/sema_skeleton.ore

missing_import_file="$TMP_DIR/missing_import.ore"
printf 'missing :: @import("missing_dependency.ore")\n' >"$missing_import_file"

duplicate_import_file="$TMP_DIR/duplicate_import.ore"
cat >"$duplicate_import_file" <<ORE
math_a :: @import("$PROJECT_ROOT/examples/import_math.ore")
math_b :: @import("$PROJECT_ROOT/examples/import_math.ore")

sum :: math_a.answer + math_b.answer
ORE

run_failure_contains "missing root reports path diagnostic" \
    "could not resolve path" \
    "$ORE" --no-color --quiet "$TMP_DIR/missing_root.ore"
run_failure_contains "missing import reports path diagnostic" \
    "could not resolve import path 'missing_dependency.ore'" \
    "$ORE" --no-color --quiet "$missing_import_file"
run_success "duplicate imports reuse cached module" \
    "$ORE" --quiet "$duplicate_import_file"
run_failure_contains "import_missing_field reports missing member" \
    "module 'math' has no member 'missing'" \
    "$ORE" --no-color --quiet examples/import_missing_field.ore
run_failure_contains "import_cycle_a reports circular import" \
    "circular import" \
    "$ORE" --no-color --quiet examples/import_cycle_a.ore
run_failure_contains "parse reports unexpected token" \
    "unexpected token RBrace" \
    "$ORE" --no-color --quiet examples/parse.ore

printf '\n%d passed, %d failed\n' "$PASS_COUNT" "$FAIL_COUNT"

if [ "$FAIL_COUNT" -ne 0 ]; then
    exit 1
fi

exit 0