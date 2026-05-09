#include "coerce.h"

#include <stdio.h>

#include "../../diag/diag.h"
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
  diag_error(&s->diags, span, "expected %s, got %s%s%s", to_str, from_str,
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
  if (from->kind != TY_PTR || to->kind != TY_PTR) return false;
  if (from->ptr.elem != to->ptr.elem) return false;
  // Mut → const is fine; const → mut is not.
  if (!from->ptr.is_const && to->ptr.is_const) return true;
  // Same constness is just structural equality (already caught by
  // from==to via interning); reaching here means const → mut.
  return false;
}

// `[]T → []const T` and identical-constness slice match. Same shape as
// the pointer rule — drop write capability, don't change element type.
static bool coerce_slice_to_slice(const struct Type *from, const struct Type *to) {
  if (from->kind != TY_SLICE || to->kind != TY_SLICE) return false;
  if (from->slice.elem != to->slice.elem) return false;
  if (!from->slice.is_const && to->slice.is_const) return true;
  return false;
}

// `[^]T → [^]const T` (mirror of slice / single-ptr).
static bool coerce_many_ptr_to_many_ptr(const struct Type *from,
                                        const struct Type *to) {
  if (from->kind != TY_MANY_PTR || to->kind != TY_MANY_PTR) return false;
  if (from->many_ptr.elem != to->many_ptr.elem) return false;
  if (!from->many_ptr.is_const && to->many_ptr.is_const) return true;
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
  if (from->kind != TY_PTR || to->kind != TY_SLICE) return false;
  const struct Type *pointee = from->ptr.elem;
  if (!pointee || pointee->kind != TY_ARRAY) return false;
  if (pointee->array.elem != to->slice.elem) return false;
  // pointer-const flows through: `^const [N]T → []const T` ok,
  // `^const [N]T → []T` rejected.
  if (from->ptr.is_const && !to->slice.is_const) return false;
  return true;
}

// `^[N]T → [^]T` / `^[N]T → [^]const T`. Pointer to array decays to
// a many-pointer at the same element type; same constness rules.
static bool coerce_array_ptr_to_many_ptr(const struct Type *from,
                                         const struct Type *to) {
  if (from->kind != TY_PTR || to->kind != TY_MANY_PTR) return false;
  const struct Type *pointee = from->ptr.elem;
  if (!pointee || pointee->kind != TY_ARRAY) return false;
  if (pointee->array.elem != to->many_ptr.elem) return false;
  if (from->ptr.is_const && !to->many_ptr.is_const) return false;
  return true;
}

// Internal driver. `emit` controls whether diagnostics fire; the
// public `coerce` wrapper passes true. Used internally with false to
// speculatively check rules (e.g., optional lift) without producing
// duplicate diagnostics when the speculative attempt fails.
static bool coerce_check(struct Sema *s, struct Type *from, struct Type *to,
                         struct ConstValue value, struct Span span,
                         bool emit) {
  if (!s) return false;
  // Both error: silent ok (errors already flagged upstream).
  if (!from || !to) return true;
  if (from->kind == TY_ERROR || to->kind == TY_ERROR) return true;

  // Pointer-equal types short-circuit. Compound types are interned,
  // so this catches every "structurally identical" case.
  if (from == to) return true;

  // `noreturn` is the bottom type — an expression of type noreturn
  // diverges (return / break / continue / panic) and never produces a
  // value, so it trivially "satisfies" any expected type. Mirrors
  // Zig's rule for `noreturn`. Without this, a fn body whose tail is
  // a `return` would error on the return-type check.
  if (from->kind == TY_NORETURN) return true;

  // Pointer / slice / array-ptr variance — drop mutability, decay
  // array-ptr to slice / many-ptr. See `coerce_*` helpers above for
  // exact rules. Each is a pure structural check.
  if (coerce_ptr_to_ptr(from, to))             return true;
  if (coerce_slice_to_slice(from, to))         return true;
  if (coerce_many_ptr_to_many_ptr(from, to))   return true;
  if (coerce_array_ptr_to_slice(from, to))     return true;
  if (coerce_array_ptr_to_many_ptr(from, to))  return true;

  // Optional lift: `T → ?T`. Any value coercible to ?T's element type
  // flows into the optional. Speculative — pass emit=false so the
  // recursive failure path doesn't emit a misleading error against
  // the unwrapped element. If the lift succeeds we return true; if
  // it fails, the outer (this-frame) error path emits with the
  // original `?T` target so the user sees the right type spelling.
  if (to->kind == TY_OPTIONAL) {
    if (coerce_check(s, from, to->optional.elem, value, span,
                     /*emit=*/false))
      return true;
  }

  // Comptime → concrete numeric: range-check via fits_in.
  if (from->kind == TY_COMPTIME_INT) {
    if (!type_is_int(to)) {
      if (emit) emit_coerce_error(s, span, from, to, NULL);
      return false;
    }
    // `to` is comptime_int — handled by from==to above when interned.
    if (to->kind == TY_COMPTIME_INT) return true;

    if (value.kind == CONST_INT) {
      const char *lo = NULL, *hi = NULL;
      if (fits_in(value, to, &lo, &hi)) return true;
      if (emit) {
        char vbuf[64];
        const_value_to_str(value, vbuf, sizeof(vbuf));
        if (lo && hi) {
          diag_error(&s->diags, span,
                     "value %s does not fit in %s (range %s..%s)", vbuf,
                     type_name(to), lo, hi);
        } else {
          diag_error(&s->diags, span, "value %s is not representable in %s",
                     vbuf, type_name(to));
        }
      }
      return false;
    }
    // No const value but compatible kinds — accept structurally.
    // The const_eval layer is responsible for range checks when a
    // value is known. This branch hits when a comptime_int flows
    // through a non-constant-foldable path, which today doesn't
    // happen but the door is open for future const-folding gaps.
    return true;
  }

  if (from->kind == TY_COMPTIME_FLOAT) {
    if (to->kind != TY_F32 && to->kind != TY_F64 &&
        to->kind != TY_COMPTIME_FLOAT) {
      if (emit) emit_coerce_error(s, span, from, to, NULL);
      return false;
    }
    if (value.kind == CONST_FLOAT) {
      const char *lo = NULL, *hi = NULL;
      if (fits_in(value, to, &lo, &hi)) return true;
      if (emit) {
        char vbuf[64];
        const_value_to_str(value, vbuf, sizeof(vbuf));
        if (lo && hi) {
          diag_error(&s->diags, span,
                     "value %s does not fit in %s (range %s..%s)", vbuf,
                     type_name(to), lo, hi);
        } else {
          diag_error(&s->diags, span, "value %s is not representable in %s",
                     vbuf, type_name(to));
        }
      }
      return false;
    }
    return true;
  }

  // Concrete-to-concrete: exact match only, no implicit widening.
  // Different bit-width or signedness requires an explicit cast at
  // the call site. This is intentional — silent widening hides
  // overflow paths.
  if (emit) emit_coerce_error(s, span, from, to, NULL);
  return false;
}

bool coerce(struct Sema *s, struct Type *from, struct Type *to,
            struct ConstValue value, struct Span span) {
  return coerce_check(s, from, to, value, span, /*emit=*/true);
}
