#include "coerce.h"

#include <stdio.h>

#include "db/diag/diag.h"
#include "../sema.h"
#include "fits.h"
#include "type.h"

// Forward decl into display.h to avoid an include cycle here.
const char *type_to_string(struct Sema *s, const struct Type *t, char *buf,
                           size_t buflen);

static void emit_coerce_error(struct Sema *s, struct Span span,
                              struct Type *from, struct Type *to,
                              const char *reason) {
  char from_buf[128], to_buf[128];
  const char *from_str = type_to_string(s, from, from_buf, sizeof(from_buf));
  const char *to_str = type_to_string(s, to, to_buf, sizeof(to_buf));
  diag_emit(s, span, "expected %s, got %s%s%s", to_str, from_str,
            reason ? " — " : "", reason ? reason : "");
}

// Variance rules for compound types — each returns true if `from` can
// safely coerce to `to` without a runtime cast, false otherwise. None
// of these allocate: they decide structurally and (via interning)
// rely on pointer equality of element types.
//
// Mirrors Zig's `coerceInMemoryAllowed*` family in src/Sema.zig. The
// shared invariant: read-only is wider than read-write, length-known
// is wider than length-erased. So `^T → ^const T` and `[]T → []const T`
// are always allowed (drop write capability), but the reverse needs an
// explicit cast.

// `^T  → ^const T` and identical-constness pointer match. Pointer-to-
// pointer with different elem types is rejected — callers can only
// drop mutability, not change what the pointer aims at.
static bool coerce_ptr_to_ptr(const struct Type *from, const struct Type *to) {
  if (from->kind != TY_PTR || to->kind != TY_PTR)
    return false;
  if (from->ptr.elem != to->ptr.elem)
    return false;
  // Mut → const is fine; const → mut is not.
  if (!from->ptr.is_const && to->ptr.is_const)
    return true;
  // Same constness is just structural equality (already caught by
  // from==to via interning); reaching here means const → mut.
  return false;
}

// `[]T → []const T` and identical-constness slice match. Same shape as
// the pointer rule — drop write capability, don't change element type.
static bool coerce_slice_to_slice(const struct Type *from,
                                  const struct Type *to) {
  if (from->kind != TY_SLICE || to->kind != TY_SLICE)
    return false;
  if (from->slice.elem != to->slice.elem)
    return false;
  if (!from->slice.is_const && to->slice.is_const)
    return true;
  return false;
}

// `[^]T → [^]const T` (mirror of slice / single-ptr).
static bool coerce_many_ptr_to_many_ptr(const struct Type *from,
                                        const struct Type *to) {
  if (from->kind != TY_MANY_PTR || to->kind != TY_MANY_PTR)
    return false;
  if (from->many_ptr.elem != to->many_ptr.elem)
    return false;
  if (!from->many_ptr.is_const && to->many_ptr.is_const)
    return true;
  return false;
}

// `^[N]T → []T` / `^[N]T → []const T`. A pointer to an array carries
// its length in the type, so dropping to a slice is a length-preserving
// reinterpretation: the slice points into the same N elements with
// runtime length N. Mirrors Zig's `coerceArrayPtrToSlice`.
//
// Constness narrows: `^[N]T → []T` and `^[N]T → []const T` both work,
// but `^const [N]T → []T` (drop const) does not.
static bool coerce_array_ptr_to_slice(const struct Type *from,
                                      const struct Type *to) {
  if (from->kind != TY_PTR || to->kind != TY_SLICE)
    return false;
  const struct Type *pointee = from->ptr.elem;
  if (!pointee || pointee->kind != TY_ARRAY)
    return false;
  if (pointee->array.elem != to->slice.elem)
    return false;
  // pointer-const flows through: `^const [N]T → []const T` ok,
  // `^const [N]T → []T` rejected.
  if (from->ptr.is_const && !to->slice.is_const)
    return false;
  return true;
}

// `^[N]T → [^]T` / `^[N]T → [^]const T`. Pointer to array decays to
// a many-pointer at the same element type; same constness rules.
static bool coerce_array_ptr_to_many_ptr(const struct Type *from,
                                         const struct Type *to) {
  if (from->kind != TY_PTR || to->kind != TY_MANY_PTR)
    return false;
  const struct Type *pointee = from->ptr.elem;
  if (!pointee || pointee->kind != TY_ARRAY)
    return false;
  if (pointee->array.elem != to->many_ptr.elem)
    return false;
  if (from->ptr.is_const && !to->many_ptr.is_const)
    return false;
  return true;
}

// Result of the pure structural+range check. The `coerce` wrapper
// switches on this to decide which diagnostic (if any) to emit.
// Pre-B19 this was implicit in a threaded `emit` flag; the flag is
// gone — emission is fully owned by the wrapper, and the predicate
// is Sema-free and side-effect-free, safe to call speculatively
// (e.g., from the optional-lift recursion).
typedef enum {
  COERCE_OK = 0,
  COERCE_FAIL_TYPE,  // structural type mismatch (wrong kind, wrong elem, etc.)
  COERCE_FAIL_RANGE, // comptime value doesn't fit in target's numeric range
} CoerceResult;

// Pure structural + range predicate. No diagnostics, no Sema
// mutation. Returns OK when `from` (with the optional comptime
// `value`) is coercible to `to`; otherwise a category telling the
// caller which diagnostic to emit. Recursion (e.g., optional lift)
// stays inside this function.
static CoerceResult coerce_structural(struct Type *from, struct Type *to,
                                      struct ConstValue value) {
  // Both error: silent ok (errors already flagged upstream).
  if (!from || !to)
    return COERCE_OK;
  if (from->kind == TY_ERROR || to->kind == TY_ERROR)
    return COERCE_OK;

  // Pointer-equal types short-circuit. Compound types are interned,
  // so this catches every "structurally identical" case.
  if (from == to)
    return COERCE_OK;

  // `noreturn` is the bottom type — an expression of type noreturn
  // diverges (return / break / continue / panic) and never produces a
  // value, so it trivially "satisfies" any expected type. Mirrors
  // Zig's rule for `noreturn`.
  if (from->kind == TY_NORETURN)
    return COERCE_OK;

  // Pointer / slice / array-ptr variance.
  if (coerce_ptr_to_ptr(from, to))
    return COERCE_OK;
  if (coerce_slice_to_slice(from, to))
    return COERCE_OK;
  if (coerce_many_ptr_to_many_ptr(from, to))
    return COERCE_OK;
  if (coerce_array_ptr_to_slice(from, to))
    return COERCE_OK;
  if (coerce_array_ptr_to_many_ptr(from, to))
    return COERCE_OK;

  // Optional lift: `T → ?T`. Speculative recursion — if the inner
  // check fails, fall through to the outer failure path so the
  // diagnostic spells the original `?T` target, not the unwrapped
  // element.
  if (to->kind == TY_OPTIONAL) {
    if (coerce_structural(from, to->optional.elem, value) == COERCE_OK)
      return COERCE_OK;
  }

  // `nil → ?T` and `nil → ^T / [^]T / []T`. The TY_NIL singleton is
  // coerce-compatible with these targets only; it can't be stored or
  // used arithmetically.
  if (from->kind == TY_NIL) {
    switch (to->kind) {
    case TY_OPTIONAL:
    case TY_PTR:
    case TY_MANY_PTR:
    case TY_SLICE:
      return COERCE_OK;
    default:
      break;
    }
  }

  // Comptime int → concrete numeric: range-check via fits_in.
  if (from->kind == TY_COMPTIME_INT) {
    if (!type_is_int(to))
      return COERCE_FAIL_TYPE;
    if (to->kind == TY_COMPTIME_INT)
      return COERCE_OK;
    if (value.kind == CONST_INT) {
      return fits_in(value, to, NULL, NULL) ? COERCE_OK : COERCE_FAIL_RANGE;
    }
    // Compatible kinds, no const value — accept structurally. The
    // const_eval layer is responsible for range checks when a value
    // is known.
    return COERCE_OK;
  }

  if (from->kind == TY_COMPTIME_FLOAT) {
    if (to->kind != TY_F32 && to->kind != TY_F64 &&
        to->kind != TY_COMPTIME_FLOAT)
      return COERCE_FAIL_TYPE;
    if (value.kind == CONST_FLOAT) {
      return fits_in(value, to, NULL, NULL) ? COERCE_OK : COERCE_FAIL_RANGE;
    }
    return COERCE_OK;
  }

  // Concrete-to-concrete: exact match only, no implicit widening.
  return COERCE_FAIL_TYPE;
}

// Emit the right diagnostic for a range failure. Re-derives `lo`/`hi`
// by calling fits_in a second time — cheap (numeric range comparison),
// keeps coerce_structural Sema-free.
static void emit_range_error(struct Sema *s, struct Span span, struct Type *to,
                             struct ConstValue value) {
  const char *lo = NULL, *hi = NULL;
  (void)fits_in(value, to, &lo, &hi);
  char vbuf[64];
  const_value_to_str(value, vbuf, sizeof(vbuf));
  if (lo && hi) {
    diag_emit(s, span, "value %s does not fit in %s (range %s..%s)", vbuf,
              type_name(to), lo, hi);
  } else {
    diag_emit(s, span, "value %s is not representable in %s", vbuf,
              type_name(to));
  }
}

bool coerce(struct Sema *s, struct Type *from, struct Type *to,
            struct ConstValue value, struct Span span) {
  if (!s)
    return false;
  CoerceResult r = coerce_structural(from, to, value);
  if (r == COERCE_OK)
    return true;
  if (r == COERCE_FAIL_RANGE) {
    emit_range_error(s, span, to, value);
    return false;
  }
  // COERCE_FAIL_TYPE
  emit_coerce_error(s, span, from, to, NULL);
  return false;
}
