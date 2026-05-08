#!/bin/sh

set -u

PROJECT_ROOT=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
ORE="$PROJECT_ROOT/ore"

# CC and TEST_CFLAGS are normally set by the Makefile.
# Defaults here let you run `sh tools/test.sh` directly during development.
: "${CC:=zig cc}"
: "${TEST_CFLAGS:=-std=c17 -Wall -Isrc}"

TEST_CFLAGS=${TEST_CFLAGS:-"-std=c23 -Wall -Isrc ${NIX_LDFLAGS} -lasan -fsanitize=address -g"}
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

# Successful build (exit 0) whose stderr contains the given substring —
# used for warning regressions (warnings don't fail the build).
run_success_warns() {
    test_name=$1
    expected=$2
    shift 2
    TEST_COUNT=$((TEST_COUNT + 1))
    stdout_file="$TMP_DIR/stdout.$TEST_COUNT"
    stderr_file="$TMP_DIR/stderr.$TEST_COUNT"

    if ! "$@" >"$stdout_file" 2>"$stderr_file"; then
        status=$?
        fail "$test_name"
        printf '  command:'
        printf ' %s' "$@"
        printf '\n'
        printf '  expected: exit 0 and stderr containing: %s\n' "$expected"
        printf '  actual: exit %d\n' "$status"
        print_file stdout "$stdout_file"
        print_file stderr "$stderr_file"
        return
    fi

    if grep -Fq "$expected" "$stderr_file"; then
        pass "$test_name"
        return
    fi

    fail "$test_name"
    printf '  command:'
    printf ' %s' "$@"
    printf '\n'
    printf '  expected: exit 0 and stderr containing: %s\n' "$expected"
    printf '  actual: exit 0 (substring missing)\n'
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
if ! $CC $TEST_CFLAGS tools/sema_type_test.c src/sema/type.c src/sema/query.c src/common/vec.c src/common/stringpool.c src/common/arena.c src/common/hashmap.c -o "$SEMA_TYPE_TEST"; then
    printf 'FAIL sema type smoke test build\n'
    exit 1
fi

printf 'Building sema const-eval smoke test...\n'
CONST_EVAL_SRCS="tools/sema_const_eval_test.c \
    src/sema/const_eval.c src/sema/layout.c src/sema/target.c \
    src/sema/type.c src/sema/query.c \
    src/common/vec.c src/common/stringpool.c src/common/arena.c src/common/hashmap.c"
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
loop_forever :: fn() -> void
    loop
        break

loop_while :: fn(limit: i32) -> void
    loop (limit > 0)
        break

loop_c_style :: fn(limit: i32) -> void
    loop (i := 0; i < limit; i++)
        if (i == 1)
            continue

loop_capture :: fn(item: ?i32) -> i32
    loop (item) |value|
        value
ORE

wildcard_file="$TMP_DIR/wildcard.ore"
cat >"$wildcard_file" <<ORE
wildcard_uses :: fn(x: i32) -> i32
    switch x
        1 => 10
        _ => 99

discard_call :: fn() -> void
    _ = wildcard_uses(0)
ORE

discarded_value_file="$TMP_DIR/discarded_value.ore"
cat >"$discarded_value_file" <<ORE
returns_i32 :: fn() -> i32
    42

bad_discard :: fn() -> void
    returns_i32()
    void
ORE

missing_struct_field_file="$TMP_DIR/missing_struct_field.ore"
cat >"$missing_struct_field_file" <<ORE
Point :: struct
    x: i32
    y: i32

bad_field :: fn() -> i32
    Point.z
ORE

enum_match_file="$TMP_DIR/enum_match.ore"
cat >"$enum_match_file" <<ORE
Color :: enum
    Red
    Green
    Blue

label :: fn(c: Color) -> i32
    switch c
        .Red   => 0
        .Green => 1
        .Blue  => 2

with_wildcard :: fn(c: Color) -> i32
    switch c
        .Red => 0
        _    => 99
ORE

enum_unknown_variant_file="$TMP_DIR/enum_unknown_variant.ore"
cat >"$enum_unknown_variant_file" <<ORE
Color :: enum
    Red
    Green

bad :: fn(c: Color) -> i32
    switch c
        .Red    => 0
        .Yellow => 1
        .Green  => 2
ORE

enum_non_exhaustive_file="$TMP_DIR/enum_non_exhaustive.ore"
cat >"$enum_non_exhaustive_file" <<ORE
Color :: enum
    Red
    Green
    Blue

bad :: fn(c: Color) -> i32
    switch c
        .Red   => 0
        .Green => 1
ORE

enum_bad_explicit_value_file="$TMP_DIR/enum_bad_explicit_value.ore"
cat >"$enum_bad_explicit_value_file" <<ORE
Tag :: enum
    A = true
ORE

enum_duplicate_variant_file="$TMP_DIR/enum_duplicate_variant.ore"
cat >"$enum_duplicate_variant_file" <<ORE
Color :: enum
    Red
    Green
    Red
ORE

unknown_builtin_file="$TMP_DIR/unknown_builtin.ore"
cat >"$unknown_builtin_file" <<ORE
bad :: fn() -> i32
    @notabuiltin(0)
ORE

return_type_builtin_file="$TMP_DIR/return_type_builtin.ore"
cat >"$return_type_builtin_file" <<ORE
wrap :: fn(action: fn() -> i32) -> @returnType(action)
    action()
ORE

return_type_non_fn_file="$TMP_DIR/return_type_non_fn.ore"
cat >"$return_type_non_fn_file" <<ORE
bad :: fn() -> i32
    x :: @returnType(42)
    0
ORE

break_outside_file="$TMP_DIR/break_outside.ore"
cat >"$break_outside_file" <<ORE
bad_break :: fn() -> void
    break
ORE

continue_outside_file="$TMP_DIR/continue_outside.ore"
cat >"$continue_outside_file" <<ORE
bad_continue :: fn() -> void
    continue
ORE

break_header_file="$TMP_DIR/break_header.ore"
cat >"$break_header_file" <<ORE
bad_header_break :: fn() -> void
    loop (break)
        0
ORE

nested_break_file="$TMP_DIR/nested_break.ore"
cat >"$nested_break_file" <<ORE
bad_nested_break :: fn() -> void
    loop
        nested :: fn() -> void
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
bad_return :: fn() -> i32
    true
ORE

call_arg_mismatch_file="$TMP_DIR/call_arg_mismatch.ore"
cat >"$call_arg_mismatch_file" <<ORE
id :: fn(x: i32) -> i32
    x

bad_call :: id(true)
ORE

call_arity_mismatch_file="$TMP_DIR/call_arity_mismatch.ore"
cat >"$call_arity_mismatch_file" <<ORE
id :: fn(x: i32) -> i32
    x

bad_call :: id()
ORE

product_field_mismatch_file="$TMP_DIR/product_field_mismatch.ore"
cat >"$product_field_mismatch_file" <<ORE
Buffer :: struct
    data : []u8

bad_buffer :: fn() -> Buffer
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
run_success "wildcard pattern + discard assign type-check" \
    "$ORE" --quiet "$wildcard_file"
run_failure_contains "discarded non-void result reports diagnostic" \
    "unused result of type i32" \
    "$ORE" --no-color --quiet "$discarded_value_file"
run_failure_contains "missing struct field reports diagnostic at resolution" \
    "struct 'Point' has no member 'z'" \
    "$ORE" --no-color --quiet "$missing_struct_field_file"
run_success "exhaustive enum switch + wildcard escape type-checks" \
    "$ORE" --quiet "$enum_match_file"
run_failure_contains "unknown enum variant in switch reports diagnostic" \
    "enum 'Color' has no variant 'Yellow'" \
    "$ORE" --no-color --quiet "$enum_unknown_variant_file"
run_failure_contains "non-exhaustive enum switch reports diagnostic" \
    "switch on enum 'Color' is not exhaustive: variant 'Blue' is unhandled" \
    "$ORE" --no-color --quiet "$enum_non_exhaustive_file"
run_failure_contains "non-integer enum variant value reports diagnostic" \
    "enum variant value must be an integer" \
    "$ORE" --no-color --quiet "$enum_bad_explicit_value_file"
run_failure_contains "duplicate enum variant reports diagnostic" \
    "'Red' is already defined in this scope" \
    "$ORE" --no-color --quiet "$enum_duplicate_variant_file"
run_failure_contains "unknown @-builtin in expression position reports diagnostic" \
    "unknown comptime builtin '@notabuiltin'" \
    "$ORE" --no-color --quiet "$unknown_builtin_file"
run_success "@returnType resolves to a function's return type" \
    "$ORE" --quiet "$return_type_builtin_file"
run_failure_contains "@returnType on non-function reports diagnostic" \
    "@returnType expects a function" \
    "$ORE" --no-color --quiet "$return_type_non_fn_file"
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
bad :: fn(x: i32) -> i32
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

generic_alloc_file="$TMP_DIR/generic_alloc.ore"
cat >"$generic_alloc_file" <<'ORE'
alloc :: fn(comptime t: type, count: usize) -> []t
    nil

a :: alloc(u8, 32)
b :: alloc(u32, 8)
ORE

generic_runtime_arg_file="$TMP_DIR/generic_runtime_arg.ore"
cat >"$generic_runtime_arg_file" <<'ORE'
alloc :: fn(comptime t: type, count: usize) -> []t
    nil

bad :: fn(unknown: type, n: usize) -> []u8
    alloc(unknown, n)
ORE

run_success "generic alloc instantiates per call site" \
    "$ORE" --quiet "$generic_alloc_file"
run_failure_contains "non-comptime argument to comptime param reports diagnostic" \
    "must be known at compile time" \
    "$ORE" --no-color --quiet "$generic_runtime_arg_file"

effect_op_call_file="$TMP_DIR/effect_op_call.ore"
cat >"$effect_op_call_file" <<'ORE'
Exn :: effect
    raise :: fn() -> void

f :: fn() <Exn> -> void
    raise()
ORE

effect_propagate_file="$TMP_DIR/effect_propagate.ore"
cat >"$effect_propagate_file" <<'ORE'
Exn :: effect
    raise :: fn() -> void

h :: fn() <Exn> -> void
    raise()

f :: fn() -> void
    h()
ORE

effect_open_row_file="$TMP_DIR/effect_open_row.ore"
cat >"$effect_open_row_file" <<'ORE'
Exn :: effect
    raise :: fn() -> void

h :: fn() <Exn> -> void
    raise()

g :: fn() <| e> -> void
    h()
ORE

run_success "declared effect satisfies inferred effect" \
    "$ORE" --quiet "$effect_op_call_file"
run_failure_contains "undeclared effect propagates from callee and is rejected" \
    "performs effect 'Exn' but its signature does not declare it" \
    "$ORE" --no-color --quiet "$effect_propagate_file"
run_success "open effect row absorbs callee effects" \
    "$ORE" --quiet "$effect_open_row_file"

evidence_stack_file="$TMP_DIR/evidence_stack.ore"
cat >"$evidence_stack_file" <<'ORE'
Allocator :: scoped effect
    alloc :: fn(comptime t: type, count: usize) -> []t

Exn :: effect
    raise :: fn() -> void

debug_allocator :: fn(comptime s: Scope, action: fn() <Allocator(s)> -> i32) -> i32
    with handler
        alloc :: fn(t, count)
            nil

    action()

exn :: fn(action: fn() <Exn> -> i32) -> i32
    with handler
        raise :: fn()
            0

    action()

main :: fn() -> i32
    with exn
    with debug_allocator

    a :: alloc(u8, 32)
    raise()
    0
ORE

run_success "evidence vector dump runs without errors" \
    "$ORE" --quiet --dump-evidence "$evidence_stack_file"

edge_u8="$TMP_DIR/edge_u8.ore"
printf 'good : u8 = 255\n' >"$edge_u8"
run_success "u8 = 255 is valid" \
    "$ORE" --quiet "$edge_u8"

edge_i32_min="$TMP_DIR/edge_i32_min.ore"
printf 'good : i32 = -2147483648\n' >"$edge_i32_min"
run_success "i32 minimum is valid" \
    "$ORE" --quiet "$edge_i32_min"

panic_arity_file="$TMP_DIR/panic_arity.ore"
cat >"$panic_arity_file" <<'ORE'
Exn :: effect
    panic :: ctl(comptime E: type, variant: E, msg: []const u8) -> noreturn

bad :: fn() <Exn> -> void
    panic("oops")
ORE
run_failure_contains "panic with too few args reports arity diagnostic" \
    "expected 3 arguments but found 1" \
    "$ORE" --no-color --quiet "$panic_arity_file"

scope_infer_file="$TMP_DIR/scope_infer.ore"
cat >"$scope_infer_file" <<'ORE'
Allocator :: scoped effect
    alloc :: fn(comptime t: type, count: usize) -> []t

debug_allocator :: fn(comptime s: Scope, action: fn() <Allocator(s)> -> i32) -> i32
    with handler
        alloc :: fn(t, count)
            nil
    action()

main :: fn() -> i32
    with debug_allocator
    a :: alloc(u8, 32)
    0
ORE
run_success "comptime Scope param is inferred from active handler" \
    "$ORE" --quiet "$scope_infer_file"

with_bound_file="$TMP_DIR/with_bound.ore"
cat >"$with_bound_file" <<'ORE'
Allocator :: scoped effect
    alloc :: fn(comptime t: type, count: usize) -> []t

debug_allocator :: fn(comptime s: Scope, action: fn(arena: usize) <Allocator(s)> -> i32) -> i32
    with handler
        alloc :: fn(t, count)
            nil
    action(0)

main :: fn() -> i32
    with arena := debug_allocator
    a :: alloc(u8, arena)
    0
ORE
run_success "with x := f desugars to f(fn(x) body)" \
    "$ORE" --quiet "$with_bound_file"

handler_lifecycle_file="$TMP_DIR/handler_lifecycle.ore"
cat >"$handler_lifecycle_file" <<'ORE'
Allocator :: scoped effect
    alloc :: fn(comptime t: type, count: usize) -> []t

debug_allocator :: fn(comptime s: Scope, action: fn() <Allocator(s)> -> i32) -> i32
    with handler
        alloc :: fn(t, count)
            nil
        initially { x := 0 }
        finally 0
        return(0)

    action()

main :: fn() -> i32
    with debug_allocator
    a :: alloc(u8, 32)
    0
ORE
run_success "handler block parses initially/finally/return clauses" \
    "$ORE" --quiet "$handler_lifecycle_file"

handler_dup_file="$TMP_DIR/handler_dup.ore"
cat >"$handler_dup_file" <<'ORE'
Allocator :: scoped effect
    alloc :: fn(comptime t: type, count: usize) -> []t

debug_allocator :: fn(comptime s: Scope, action: fn() <Allocator(s)> -> i32) -> i32
    with handler
        alloc :: fn(t, count)
            nil
        initially 0
        initially 1

    action()

main :: fn() -> i32
    with debug_allocator
    a :: alloc(u8, 32)
    0
ORE
run_failure_contains "duplicate 'initially' clause is rejected" \
    "duplicate 'initially' clause" \
    "$ORE" --no-color --quiet "$handler_dup_file"

handle_target_file="$TMP_DIR/handle_target.ore"
cat >"$handle_target_file" <<'ORE'
Exn :: effect
    raise :: fn() -> void

run_action :: fn(action: fn() <Exn> -> i32) -> i32
    handle (action)
        raise :: fn()
            0
ORE
run_success "handle (target) { ops } parses with target slot set" \
    "$ORE" --quiet "$handle_target_file"

effect_fn_eq_file="$TMP_DIR/effect_fn_eq.ore"
cat >"$effect_fn_eq_file" <<'ORE'
Exn :: effect
    raise :: fn() -> void

step :: fn() <Exn> -> i32
    0

call_step :: fn(action: fn() <Exn> -> i32) <Exn> -> i32
    action()

main :: fn() <Exn> -> i32
    call_step(step)
ORE
run_success "function types with matching effect rows unify" \
    "$ORE" --quiet "$effect_fn_eq_file"

effect_fn_mismatch_file="$TMP_DIR/effect_fn_mismatch.ore"
cat >"$effect_fn_mismatch_file" <<'ORE'
Exn :: effect
    raise :: fn() -> void

pure_step :: fn() -> i32
    0

call_step :: fn(action: fn() <Exn> -> i32) <Exn> -> i32
    action()

main :: fn() <Exn> -> i32
    call_step(pure_step)
ORE
run_failure_contains "function types with differing effect rows show the row in the diagnostic" \
    "<Exn>" \
    "$ORE" --no-color --quiet "$effect_fn_mismatch_file"

handler_op_arity_file="$TMP_DIR/handler_op_arity.ore"
cat >"$handler_op_arity_file" <<'ORE'
Allocator :: scoped effect
    alloc :: fn(comptime t: type, count: usize) -> []t

debug_allocator :: fn(comptime s: Scope, action: fn() <Allocator(s)> -> i32) -> i32
    with handler
        alloc :: fn(t)
            nil
    action()

main :: fn() -> i32
    0
ORE
run_failure_contains "handler op with wrong arity reports diagnostic" \
    "takes 1 params but effect declares 2" \
    "$ORE" --no-color --quiet "$handler_op_arity_file"

handler_op_param_type_file="$TMP_DIR/handler_op_param_type.ore"
cat >"$handler_op_param_type_file" <<'ORE'
Allocator :: scoped effect
    alloc :: fn(comptime t: type, count: usize) -> []t

debug_allocator :: fn(comptime s: Scope, action: fn() <Allocator(s)> -> i32) -> i32
    with handler
        alloc :: fn(t, count: bool)
            nil
    action()

main :: fn() -> i32
    0
ORE
run_failure_contains "handler op with wrong param type reports diagnostic" \
    "but effect declares" \
    "$ORE" --no-color --quiet "$handler_op_param_type_file"

handler_op_ret_type_file="$TMP_DIR/handler_op_ret_type.ore"
cat >"$handler_op_ret_type_file" <<'ORE'
Counter :: effect
    next :: fn() -> i32

run :: fn(action: fn() <Counter> -> i32) -> i32
    with handler
        next :: fn() -> i32
            true
    action()

main :: fn() -> i32
    0
ORE
run_failure_contains "handler op body type-checked against effect's ret type" \
    "expected i32 but found bool" \
    "$ORE" --no-color --quiet "$handler_op_ret_type_file"

handler_op_ctl_vs_fn_file="$TMP_DIR/handler_op_ctl_vs_fn.ore"
cat >"$handler_op_ctl_vs_fn_file" <<'ORE'
Exn :: effect
    panic :: ctl(comptime E: type, variant: E, msg: []const u8) -> noreturn

run :: fn(action: fn() <Exn> -> i32) -> i32
    with handler
        panic :: fn(E, variant, msg)
            0
    action()

main :: fn() -> i32
    0
ORE
run_failure_contains "handler op fn-vs-ctl mismatch reports diagnostic" \
    "is declared as fn but effect declares ctl" \
    "$ORE" --no-color --quiet "$handler_op_ctl_vs_fn_file"

action_leak_file="$TMP_DIR/action_leak.ore"
cat >"$action_leak_file" <<'ORE'
Exn :: effect
    raise :: fn() -> void

emits_exn :: fn() <Exn> -> i32
    0

Allocator :: scoped effect
    alloc :: fn(comptime t: type, count: usize) -> []t

debug_allocator :: fn(comptime s: Scope, action: fn() <Allocator(s)> -> i32) -> i32
    with handler
        alloc :: fn(t, count)
            nil
    action()

bad :: fn() -> i32
    with debug_allocator
    emits_exn()
ORE
run_failure_contains "effect not declared by action signature leaks to caller" \
    "function 'bad' performs effect 'Exn' but its signature does not declare it" \
    "$ORE" --no-color --quiet "$action_leak_file"

totality_main_file="$TMP_DIR/totality_main.ore"
cat >"$totality_main_file" <<'ORE'
Exn :: effect
    raise :: fn() -> void

emits_exn :: fn() <Exn> -> i32
    0

main :: fn() -> i32
    emits_exn()
ORE
run_failure_contains "main with unhandled effect rejected (totality)" \
    "function 'main' performs effect 'Exn' but its signature does not declare it" \
    "$ORE" --no-color --quiet "$totality_main_file"


handler_multi_op_file="$TMP_DIR/handler_multi_op.ore"
cat >"$handler_multi_op_file" <<'ORE'
Allocator :: scoped effect
    alloc :: fn(comptime t: type, count: usize) -> []t
    free  :: fn(p: usize) -> void

debug_allocator :: fn(comptime s: Scope, action: fn() <Allocator(s)> -> i32) -> i32
    with handler
        alloc :: fn(t, count)
            nil
        free :: fn(p)
            0
    action()

main :: fn() -> i32
    with debug_allocator
    a :: alloc(u8, 32)
    free(0)
    0
ORE
run_success "handler with all ops of multi-op effect resolves" \
    "$ORE" --quiet "$handler_multi_op_file"

handler_missing_op_file="$TMP_DIR/handler_missing_op.ore"
cat >"$handler_missing_op_file" <<'ORE'
Allocator :: scoped effect
    alloc :: fn(comptime t: type, count: usize) -> []t
    free  :: fn(p: usize) -> void

debug_allocator :: fn(comptime s: Scope, action: fn() <Allocator(s)> -> i32) -> i32
    with handler
        alloc :: fn(t, count)
            nil
    action()

main :: fn() -> i32
    0
ORE
run_failure_contains "handler missing an op of effect reports diagnostic" \
    "handler doesn't match any effect in scope" \
    "$ORE" --no-color --quiet "$handler_missing_op_file"

handler_extra_op_file="$TMP_DIR/handler_extra_op.ore"
cat >"$handler_extra_op_file" <<'ORE'
Allocator :: scoped effect
    alloc :: fn(comptime t: type, count: usize) -> []t

debug_allocator :: fn(comptime s: Scope, action: fn() <Allocator(s)> -> i32) -> i32
    with handler
        alloc :: fn(t, count)
            nil
        bonus :: fn()
            0
    action()

main :: fn() -> i32
    0
ORE
run_failure_contains "handler with an extra op reports diagnostic" \
    "handler doesn't match any effect in scope" \
    "$ORE" --no-color --quiet "$handler_extra_op_file"

handler_ambiguous_file="$TMP_DIR/handler_ambiguous.ore"
cat >"$handler_ambiguous_file" <<'ORE'
EffectA :: effect
    ping :: fn() -> void

EffectB :: effect
    ping :: fn() -> void

run_a :: fn(action: fn() <EffectA> -> i32) -> i32
    with handler
        ping :: fn()
            0
    action()

main :: fn() -> i32
    0
ORE
run_failure_contains "ambiguous handler matching two effects reports diagnostic" \
    "handler is ambiguous" \
    "$ORE" --no-color --quiet "$handler_ambiguous_file"

handler_lifecycle_only_file="$TMP_DIR/handler_lifecycle_only.ore"
cat >"$handler_lifecycle_only_file" <<'ORE'
Allocator :: scoped effect
    alloc :: fn(comptime t: type, count: usize) -> []t

debug_allocator :: fn(comptime s: Scope, action: fn() <Allocator(s)> -> i32) -> i32
    with handler
        alloc :: fn(t, count)
            nil
        initially 0
        finally 0
        return(0)
    action()

main :: fn() -> i32
    with debug_allocator
    a :: alloc(u8, 32)
    0
ORE
run_success "lifecycle clauses don't count toward op-set" \
    "$ORE" --quiet "$handler_lifecycle_only_file"

with_escape_uses_file="$TMP_DIR/with_escape_uses.ore"
cat >"$with_escape_uses_file" <<'ORE'
Allocator :: scoped effect
    alloc :: fn(comptime t: type, count: usize) -> []t

debug :: fn(comptime s: Scope, action: fn(arena: usize) <Allocator(s)> -> i32) -> i32
    with handler
        alloc :: fn(t, count)
            nil
    action(0)

main :: fn() -> i32
    with arena := debug
    a :: alloc(u8, arena)
    0
ORE
run_success "with-bound name used inside body but not returned is fine" \
    "$ORE" --quiet "$with_escape_uses_file"

with_escape_bare_file="$TMP_DIR/with_escape_bare.ore"
cat >"$with_escape_bare_file" <<'ORE'
Allocator :: scoped effect
    alloc :: fn(comptime t: type, count: usize) -> []t

debug :: fn(comptime s: Scope, action: fn(arena: usize) <Allocator(s)> -> i32) -> i32
    with handler
        alloc :: fn(t, count)
            nil
    action(0)

main :: fn() -> i32
    with arena := debug
    arena
ORE
run_failure_contains "with-bound name returned bare reports escape diagnostic" \
    "with-bound name 'arena' cannot escape its with-block" \
    "$ORE" --no-color --quiet "$with_escape_bare_file"

with_escape_ref_file="$TMP_DIR/with_escape_ref.ore"
cat >"$with_escape_ref_file" <<'ORE'
Allocator :: scoped effect
    alloc :: fn(comptime t: type, count: usize) -> []t

debug :: fn(comptime s: Scope, action: fn(arena: usize) <Allocator(s)> -> i32) -> i32
    with handler
        alloc :: fn(t, count)
            nil
    action(0)

main :: fn() -> i32
    with arena := debug
    &arena
ORE
run_failure_contains "with-bound name '&'-returned reports escape diagnostic" \
    "with-bound name 'arena' cannot escape its with-block" \
    "$ORE" --no-color --quiet "$with_escape_ref_file"

stray_initially_file="$TMP_DIR/stray_initially.ore"
cat >"$stray_initially_file" <<'ORE'
bad :: fn() -> i32
    initially 0
    0
ORE
run_failure_contains "initially outside handler block reports diagnostic" \
    "'initially' is only valid inside a handler block" \
    "$ORE" --no-color --quiet "$stray_initially_file"

pub_keyword_file="$TMP_DIR/pub_keyword.ore"
cat >"$pub_keyword_file" <<'ORE'
pub answer :: 42
hidden :: 7
ORE
run_success "pub keyword parses on top-level binding" \
    "$ORE" --quiet "$pub_keyword_file"

pub_misuse_file="$TMP_DIR/pub_misuse.ore"
cat >"$pub_misuse_file" <<'ORE'
pub 7 + 8
ORE
run_failure_contains "pub on non-binding reports diagnostic" \
    "must precede a top-level binding" \
    "$ORE" --no-color --quiet "$pub_misuse_file"

noreturn_flow_file="$TMP_DIR/noreturn_flow.ore"
cat >"$noreturn_flow_file" <<'ORE'
trap :: fn() -> noreturn
    trap()

f :: fn(x: i32) -> i32
    if (x < 0)
        trap()
    else
        x
ORE
run_success "noreturn flows into any type as bottom" \
    "$ORE" --quiet "$noreturn_flow_file"

comptime_fold="$TMP_DIR/comptime_fold.ore"
cat >"$comptime_fold" <<'ORE'
N :: 1024 * 1024
X :: 42
M :: N + X
ORE
run_success "comptime fold of arithmetic decl values" \
    "$ORE" --quiet "$comptime_fold"

optional_distinct_file="$TMP_DIR/optional_distinct.ore"
cat >"$optional_distinct_file" <<'ORE'
Header :: struct
    size : usize

bad :: fn(opt: ?^Header) -> ^Header
    opt
ORE
run_failure_contains "optional pointer not assignable to non-optional" \
    "expected *Header but found ?*Header" \
    "$ORE" --no-color --quiet "$optional_distinct_file"

deref_field_file="$TMP_DIR/deref_field.ore"
cat >"$deref_field_file" <<'ORE'
Header :: struct
    size : usize

read_size :: fn(h: ^Header) -> usize
    h^.size
ORE
run_success "pointer-deref then field access types correctly" \
    "$ORE" --quiet "$deref_field_file"

index_bad_idx_file="$TMP_DIR/index_bad_idx.ore"
cat >"$index_bad_idx_file" <<'ORE'
take :: fn(buf: []u8) -> u8
    buf[true]
ORE
run_failure_contains "non-integer index reports diagnostic" \
    "index expression must be an integer" \
    "$ORE" --no-color --quiet "$index_bad_idx_file"

index_bad_obj_file="$TMP_DIR/index_bad_obj.ore"
cat >"$index_bad_obj_file" <<'ORE'
take :: fn(x: i32) -> i32
    x[0]
ORE
run_failure_contains "indexing non-array reports diagnostic" \
    "cannot index value of type" \
    "$ORE" --no-color --quiet "$index_bad_obj_file"

return_mismatch_check_file="$TMP_DIR/return_mismatch_check.ore"
cat >"$return_mismatch_check_file" <<'ORE'
bad :: fn(x: i32) -> i32
    return true
ORE
run_failure_contains "return value type-checked against fn return type" \
    "expected i32 but found bool" \
    "$ORE" --no-color --quiet "$return_mismatch_check_file"

return_no_value_file="$TMP_DIR/return_no_value.ore"
cat >"$return_no_value_file" <<'ORE'
bad :: fn() -> i32
    return
ORE
run_failure_contains "return with no value diagnoses against non-void return" \
    "return without value but function expects" \
    "$ORE" --no-color --quiet "$return_no_value_file"

loop_capture_bad_file="$TMP_DIR/loop_capture_bad.ore"
cat >"$loop_capture_bad_file" <<'ORE'
bad :: fn(plain: i32) -> i32
    loop (plain) |v|
        return v
ORE
run_failure_contains "loop capture on non-optional reports diagnostic" \
    "loop capture |x| requires an optional condition" \
    "$ORE" --no-color --quiet "$loop_capture_bad_file"

orelse_bad_file="$TMP_DIR/orelse_bad.ore"
cat >"$orelse_bad_file" <<'ORE'
bad :: fn(plain: i32) -> i32
    plain orelse 0
ORE
run_failure_contains "orelse on non-optional reports diagnostic" \
    "must be optional" \
    "$ORE" --no-color --quiet "$orelse_bad_file"

orelse_type_mismatch_file="$TMP_DIR/orelse_type_mismatch.ore"
cat >"$orelse_type_mismatch_file" <<'ORE'
bad :: fn(opt: ?i32) -> i32
    opt orelse true
ORE
run_failure_contains "orelse fallback type mismatch reports diagnostic" \
    "does not match unwrapped left" \
    "$ORE" --no-color --quiet "$orelse_type_mismatch_file"

assign_const_file="$TMP_DIR/assign_const.ore"
cat >"$assign_const_file" <<'ORE'
bad :: fn() -> void
    x :: 0
    x = 5
ORE
run_failure_contains "assigning to const binding reports diagnostic" \
    "cannot assign to constant binding" \
    "$ORE" --no-color --quiet "$assign_const_file"

assign_through_const_ptr_file="$TMP_DIR/assign_through_const_ptr.ore"
cat >"$assign_through_const_ptr_file" <<'ORE'
bad :: fn(p: ^const i32) -> void
    p^ = 5
ORE
run_failure_contains "writing through const pointer reports diagnostic" \
    "cannot assign through const view" \
    "$ORE" --no-color --quiet "$assign_through_const_ptr_file"

assign_through_const_slice_file="$TMP_DIR/assign_through_const_slice.ore"
cat >"$assign_through_const_slice_file" <<'ORE'
bad :: fn(s: []const u8) -> void
    s[0] = 1
ORE
run_failure_contains "writing through const slice index reports diagnostic" \
    "cannot assign through const view" \
    "$ORE" --no-color --quiet "$assign_through_const_slice_file"

mut_to_const_ptr_file="$TMP_DIR/mut_to_const_ptr.ore"
cat >"$mut_to_const_ptr_file" <<'ORE'
takes_const :: fn(p: ^const i32) -> void
    0

main :: fn() -> i32
    x : i32 = 5
    takes_const(&x)
    0
ORE
run_success "mutable pointer assignable to const pointer parameter" \
    "$ORE" --quiet "$mut_to_const_ptr_file"

const_to_mut_ptr_file="$TMP_DIR/const_to_mut_ptr.ore"
cat >"$const_to_mut_ptr_file" <<'ORE'
takes_mut :: fn(p: ^i32) -> void
    0

main :: fn() -> i32
    p : ^const i32 = nil
    takes_mut(p)
    0
ORE
run_failure_contains "const pointer not assignable to mut pointer parameter" \
    "*const i32" \
    "$ORE" --no-color --quiet "$const_to_mut_ptr_file"

two_scoped_effects_file="$TMP_DIR/two_scoped_effects.ore"
cat >"$two_scoped_effects_file" <<'ORE'
Allocator :: scoped effect
    alloc :: fn(comptime t: type, count: usize) -> []t

Reader :: scoped effect
    read :: fn(n: usize) -> []u8

debug_alloc :: fn(comptime s: Scope, action: fn() <Allocator(s)> -> i32) -> i32
    with handler
        alloc :: fn(t, count)
            nil
    action()

stub_reader :: fn(comptime s: Scope, action: fn() <Reader(s)> -> i32) -> i32
    with handler
        read :: fn(n)
            nil
    action()

main :: fn() -> i32
    with debug_alloc
    with stub_reader
    a :: alloc(u8, 32)
    b :: read(8)
    0
ORE
run_success "two distinct scoped effects active simultaneously route correctly" \
    "$ORE" --quiet "$two_scoped_effects_file"

transitive_const_field_file="$TMP_DIR/transitive_const_field.ore"
cat >"$transitive_const_field_file" <<'ORE'
Shape :: struct
    width: i32

bad :: fn(p: ^const Shape) -> void
    p.width = 5
ORE
run_failure_contains "writing to field through const pointer reports diagnostic" \
    "cannot assign through const view" \
    "$ORE" --no-color --quiet "$transitive_const_field_file"

const_slice_display_file="$TMP_DIR/const_slice_display.ore"
cat >"$const_slice_display_file" <<'ORE'
takes_mut_slice :: fn(s: []u8) -> void
    0

main :: fn() -> i32
    s : []const u8 = nil
    takes_mut_slice(s)
    0
ORE
run_failure_contains "const slice display roundtrips in diagnostic" \
    "[]const u8" \
    "$ORE" --no-color --quiet "$const_slice_display_file"

assign_type_mismatch_file="$TMP_DIR/assign_type_mismatch.ore"
cat >"$assign_type_mismatch_file" <<'ORE'
bad :: fn() -> void
    x : i32 = 0
    x = true
ORE
run_failure_contains "assigning wrong type reports diagnostic" \
    "cannot assign bool to i32" \
    "$ORE" --no-color --quiet "$assign_type_mismatch_file"

array_elem_bad_file="$TMP_DIR/array_elem_bad.ore"
cat >"$array_elem_bad_file" <<'ORE'
bad :: [3]i32{1, true, 3}
ORE
run_failure_contains "array literal element type-checked" \
    "expected i32 but found bool" \
    "$ORE" --no-color --quiet "$array_elem_bad_file"

switch_pattern_bad_file="$TMP_DIR/switch_pattern_bad.ore"
cat >"$switch_pattern_bad_file" <<'ORE'
classify :: fn(x: i32) -> i32
    switch x
        0 => 100
        true => 200
ORE
run_failure_contains "switch pattern type-checked against scrutinee" \
    "switch pattern type bool does not match scrutinee i32" \
    "$ORE" --no-color --quiet "$switch_pattern_bad_file"

# Each in test.sh
overflow_u8="$TMP_DIR/overflow_u8.ore"
printf 'bad : u8 = 300\n' >"$overflow_u8"
run_failure_contains "u8 overflow on coercion" \
    "does not fit in u8" \
    "$ORE" --no-color --quiet "$overflow_u8"

overflow_neg_unsigned="$TMP_DIR/overflow_neg.ore"
printf 'bad : u32 = -1\n' >"$overflow_neg_unsigned"
run_failure_contains "negative value into unsigned" \
    "does not fit in u32" \
    "$ORE" --no-color --quiet "$overflow_neg_unsigned"

overflow_i32="$TMP_DIR/overflow_i32.ore"
printf 'bad : i32 = 5_000_000_000\n' >"$overflow_i32"
run_failure_contains "i32 overflow on coercion" \
    "does not fit in i32" \
    "$ORE" --no-color --quiet "$overflow_i32"


# NOTE: A "missing handler for inferred Scope" negative test would be useful
# but is short-circuited by the body-effect solver firing first ("main
# performs Allocator…"). Revisit once the solver supports row unification or
# context-sensitive call-site checking.

printf '\n%d passed, %d failed\n' "$PASS_COUNT" "$FAIL_COUNT"

if [ "$FAIL_COUNT" -ne 0 ]; then
    exit 1
fi

exit 0
