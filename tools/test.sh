#!/bin/sh

set -u

PROJECT_ROOT=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
ORE="$PROJECT_ROOT/ore"
CC=${CC:-clang}
TEST_CFLAGS=${TEST_CFLAGS:-"-std=c17 -Wall -Isrc -fsanitize=address -g"}
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/ore-tests.XXXXXX") || exit 1
ARENA_TEST="$TMP_DIR/arena_test"
HASHMAP_TEST="$TMP_DIR/hashmap_test"
SEMA_TYPE_TEST="$TMP_DIR/sema_type_test"
SEMA_CONST_EVAL_TEST="$TMP_DIR/sema_const_eval_test"

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

printf 'Building sema type smoke test...\n'
if ! $CC $TEST_CFLAGS tools/sema_type_test.c src/sema/type.c src/sema/query.c src/common/vec.c src/common/stringpool.c src/common/arena.c -o "$SEMA_TYPE_TEST"; then
    printf 'FAIL sema type smoke test build\n'
    exit 1
fi

printf 'Building sema const-eval smoke test...\n'
CONST_EVAL_SRCS="tools/sema_const_eval_test.c \
    src/sema/const_eval.c src/sema/layout.c src/sema/target.c \
    src/sema/type.c src/sema/query.c \
    src/common/vec.c src/common/stringpool.c src/common/arena.c"
if ! $CC $TEST_CFLAGS -DORE_CONST_EVAL_TEST $CONST_EVAL_SRCS -o "$SEMA_CONST_EVAL_TEST" 2>"$TMP_DIR/const_eval_build.err"; then
    printf 'FAIL sema const-eval smoke test build\n'
    sed 's/^/  /' "$TMP_DIR/const_eval_build.err"
    exit 1
fi
printf '\n'

run_success "arena chunk growth and marks work" \
    "$ARENA_TEST"
run_success "hashmap heap and arena modes work" \
    "$HASHMAP_TEST"
run_success "sema type helpers work" \
    "$SEMA_TYPE_TEST"
run_success "sema const-eval helpers work" \
    "$SEMA_CONST_EVAL_TEST"

run_success "import_simple succeeds" \
    "$ORE" --quiet examples/imports/import_simple.ore
run_success "effect_scope_valid succeeds" \
    "$ORE" --quiet examples/effects/effect_scope_valid.ore
run_success "sema_skeleton succeeds" \
    "$ORE" --quiet examples/sema_skeleton.ore
run_success "dump-resolve import_simple succeeds" \
    "$ORE" --dump-resolve --quiet examples/imports/import_simple.ore
run_success "dump-sema sema_skeleton succeeds" \
    "$ORE" --dump-sema --quiet examples/sema_skeleton.ore
run_success "dump-typecheck unary ops succeeds" \
    "$ORE" --dump-tyck --quiet examples/typechecking/unary_ops.ore

missing_import_file="$TMP_DIR/missing_import.ore"
printf 'missing :: @import("missing_dependency.ore")\n' >"$missing_import_file"

duplicate_import_file="$TMP_DIR/duplicate_import.ore"
cat >"$duplicate_import_file" <<ORE
math_a :: @import("$PROJECT_ROOT/examples/imports/import_math.ore")
math_b :: @import("$PROJECT_ROOT/examples/imports/import_math.ore")

sum :: math_a.answer + math_b.answer
ORE

unresolved_ident_file="$TMP_DIR/unresolved_ident.ore"
printf 'bad :: missing_name\n' >"$unresolved_ident_file"

duplicate_decl_file="$TMP_DIR/duplicate_decl.ore"
cat >"$duplicate_decl_file" <<ORE
value :: 1
value :: 2
ORE

loop_control_file="$TMP_DIR/loop_control.ore"
cat >"$loop_control_file" <<ORE
loop_forever :: fn() void
    loop
        break

loop_while :: fn(limit: i32) void
    loop (limit > 0)
        break

loop_c_style :: fn(limit: i32) void
    loop (i := 0; i < limit; i++)
        if (i == 1)
            continue

loop_capture :: fn(item: ?i32) i32
    loop (item) |value|
        value
ORE

break_outside_file="$TMP_DIR/break_outside.ore"
cat >"$break_outside_file" <<ORE
bad_break :: fn() void
    break
ORE

continue_outside_file="$TMP_DIR/continue_outside.ore"
cat >"$continue_outside_file" <<ORE
bad_continue :: fn() void
    continue
ORE

break_header_file="$TMP_DIR/break_header.ore"
cat >"$break_header_file" <<ORE
bad_header_break :: fn() void
    loop (break)
        0
ORE

nested_break_file="$TMP_DIR/nested_break.ore"
cat >"$nested_break_file" <<ORE
bad_nested_break :: fn() void
    loop
        nested :: fn() void
            break
ORE

typed_bind_mismatch_file="$TMP_DIR/typed_bind_mismatch.ore"
cat >"$typed_bind_mismatch_file" <<ORE
bad_value : i32 = true
ORE

invalid_type_annotation_file="$TMP_DIR/invalid_type_annotation.ore"
cat >"$invalid_type_annotation_file" <<ORE
not_a_type : true = 1
ORE

return_mismatch_file="$TMP_DIR/return_mismatch.ore"
cat >"$return_mismatch_file" <<ORE
bad_return :: fn() i32
    true
ORE

call_arg_mismatch_file="$TMP_DIR/call_arg_mismatch.ore"
cat >"$call_arg_mismatch_file" <<ORE
id :: fn(x: i32) i32
    x

bad_call :: id(true)
ORE

call_arity_mismatch_file="$TMP_DIR/call_arity_mismatch.ore"
cat >"$call_arity_mismatch_file" <<ORE
id :: fn(x: i32) i32
    x

bad_call :: id()
ORE

product_field_mismatch_file="$TMP_DIR/product_field_mismatch.ore"
cat >"$product_field_mismatch_file" <<ORE
Buffer :: struct
    data : []u8

bad_buffer :: fn() Buffer
    Buffer.{ .missing = nil }
ORE

mutual_recursive_types_file="$TMP_DIR/mutual_recursive_types.ore"
cat >"$mutual_recursive_types_file" <<ORE
Foo :: struct
    next : ?^Bar

Bar :: struct
    prev : ?^Foo
ORE

self_referential_value_file="$TMP_DIR/self_referential_value.ore"
printf 'bad :: bad + 1\n' >"$self_referential_value_file"

run_failure_contains "missing root reports path diagnostic" \
    "could not resolve path" \
    "$ORE" --no-color --quiet "$TMP_DIR/missing_root.ore"
run_failure_contains "missing import reports path diagnostic" \
    "could not resolve import path 'missing_dependency.ore'" \
    "$ORE" --no-color --quiet "$missing_import_file"
run_success "duplicate imports reuse cached module" \
    "$ORE" --quiet "$duplicate_import_file"
run_failure_contains "unresolved identifier reports diagnostic" \
    "is not defined in any accessible scope" \
    "$ORE" --no-color --quiet "$unresolved_ident_file"
run_failure_contains "duplicate declaration reports diagnostic" \
    "already defined in this scope" \
    "$ORE" --no-color --quiet "$duplicate_decl_file"
run_success "loop control forms resolve" \
    "$ORE" --quiet "$loop_control_file"
run_failure_contains "break outside loop reports diagnostic" \
    "break used outside of a loop" \
    "$ORE" --no-color --quiet "$break_outside_file"
run_failure_contains "continue outside loop reports diagnostic" \
    "continue used outside of a loop" \
    "$ORE" --no-color --quiet "$continue_outside_file"
run_failure_contains "break in loop header reports diagnostic" \
    "break used outside of a loop" \
    "$ORE" --no-color --quiet "$break_header_file"
run_failure_contains "break cannot cross function boundary" \
    "break used outside of a loop" \
    "$ORE" --no-color --quiet "$nested_break_file"
run_failure_contains "typed bind mismatch reports diagnostic" \
    "expected i32 but found bool" \
    "$ORE" --no-color --quiet "$typed_bind_mismatch_file"
run_failure_contains "invalid type annotation reports diagnostic" \
    "expected type expression but found bool" \
    "$ORE" --no-color --quiet "$invalid_type_annotation_file"
run_failure_contains "function return mismatch reports diagnostic" \
    "expected i32 but found bool" \
    "$ORE" --no-color --quiet "$return_mismatch_file"
run_failure_contains "call argument mismatch reports diagnostic" \
    "expected i32 but found bool" \
    "$ORE" --no-color --quiet "$call_arg_mismatch_file"
run_failure_contains "call arity mismatch reports diagnostic" \
    "expected 1 arguments but found 0" \
    "$ORE" --no-color --quiet "$call_arity_mismatch_file"
run_failure_contains "product unknown field reports diagnostic" \
    "struct 'Buffer' has no field 'missing'" \
    "$ORE" --no-color --quiet "$product_field_mismatch_file"
run_success "mutually recursive pointer types resolve" \
    "$ORE" --quiet "$mutual_recursive_types_file"
run_failure_contains "self-referential value reports circular definition" \
    "circular definition of 'bad'" \
    "$ORE" --no-color --quiet "$self_referential_value_file"
run_failure_contains "import_missing_field reports missing member" \
    "module 'math' has no member 'missing'" \
    "$ORE" --no-color --quiet examples/imports/import_missing_field.ore
run_failure_contains "import_cycle_a reports circular import" \
    "circular import" \
    "$ORE" --no-color --quiet examples/imports/import_cycle_a.ore
run_failure_contains "parse reports unexpected token" \
    "unexpected token RBrace" \
    "$ORE" --no-color --quiet examples/parse.ore

unary_not_non_bool_file="$TMP_DIR/unary_not_non_bool.ore"
printf 'bad :: !42\n' >"$unary_not_non_bool_file"

unary_neg_non_numeric_file="$TMP_DIR/unary_neg_non_numeric.ore"
printf 'bad :: -true\n' >"$unary_neg_non_numeric_file"

unary_bitnot_non_int_file="$TMP_DIR/unary_bitnot_non_int.ore"
printf 'bad :: ~3.14\n' >"$unary_bitnot_non_int_file"

unary_deref_non_pointer_file="$TMP_DIR/unary_deref_non_pointer.ore"
cat >"$unary_deref_non_pointer_file" <<'ORE'
bad :: fn(x: i32) i32
    x^
ORE

run_failure_contains "unary not on non-bool reports diagnostic" \
    "operator '!' expects bool" \
    "$ORE" --no-color --quiet "$unary_not_non_bool_file"
run_failure_contains "unary neg on non-numeric reports diagnostic" \
    "operator '-' expects numeric operand" \
    "$ORE" --no-color --quiet "$unary_neg_non_numeric_file"
run_failure_contains "unary bitnot on non-integer reports diagnostic" \
    "operator '~' expects integer operand" \
    "$ORE" --no-color --quiet "$unary_bitnot_non_int_file"
run_failure_contains "unary deref on non-pointer reports diagnostic" \
    "cannot dereference value of type" \
    "$ORE" --no-color --quiet "$unary_deref_non_pointer_file"

comptime_float_to_int_file="$TMP_DIR/comptime_float_to_int.ore"
printf 'bad : i32 = 1.5\n' >"$comptime_float_to_int_file"

run_failure_contains "comptime float not assignable to int" \
    "expected i32 but found comptimeFloat" \
    "$ORE" --no-color --quiet "$comptime_float_to_int_file"

by_value_recursive_struct_file="$TMP_DIR/by_value_recursive_struct.ore"
cat >"$by_value_recursive_struct_file" <<'ORE'
Bad :: struct
    self : Bad

x :: @sizeOf(Bad)
ORE

pointer_recursive_struct_file="$TMP_DIR/pointer_recursive_struct.ore"
cat >"$pointer_recursive_struct_file" <<'ORE'
Header :: struct
    size : usize
    next : ?^Header

s :: @sizeOf(Header)
ORE

run_failure_contains "by-value recursive struct reports layout cycle" \
    "contains itself by value" \
    "$ORE" --no-color --quiet "$by_value_recursive_struct_file"
run_success "pointer-recursive struct lays out via pointer field" \
    "$ORE" --quiet "$pointer_recursive_struct_file"

printf '\n%d passed, %d failed\n' "$PASS_COUNT" "$FAIL_COUNT"

if [ "$FAIL_COUNT" -ne 0 ]; then
    exit 1
fi

exit 0
