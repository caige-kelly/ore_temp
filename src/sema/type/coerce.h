#ifndef ORE_SEMA_TYPE_COERCE_H
#define ORE_SEMA_TYPE_COERCE_H

#include <stdbool.h>

#include "../../parser/ast.h"
#include "../eval/const_eval.h"

struct Sema;
struct Type;

// Type-to-type coercion check.
//
// Generalizes E.1's `fits_in`. Decides whether a value of type
// `from_type` can be used where `to_type` is expected, and emits a
// diagnostic on failure. Returns true on success.
//
// Cases handled:
//   - Identical types (pointer-equal, since types are interned)
//   - comptime_int → any integer type, with const-value range check
//     via `value` (CONST_INT). If `value.kind == CONST_NONE` we
//     fall back to "is comptime_int compatible with this int type"
//     which is a yes-but-untracked-overflow approximation; callers
//     should pass a concrete value when they have one.
//   - comptime_float → f32 / f64, with range check
//   - Same-kind numeric (i32 → i32, etc.) — trivially ok
//   - Mixed signedness or width-narrowing — error (require explicit
//     cast at use site)
//   - Pointer / slice / array — exact structural match for now;
//     E.3+ will relax for `^T` → `^const T`, etc.
//   - bool — no implicit numeric coercion (Zig-style strict)
//
// `span` is the source location for the diagnostic. `value` is the
// const-evaluated value (CONST_NONE if not const-known).
bool coerce(struct Sema *s, struct Type *from_type, struct Type *to_type,
            struct ConstValue value, struct Span span);

#endif
