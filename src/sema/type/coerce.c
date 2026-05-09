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

bool coerce(struct Sema *s, struct Type *from, struct Type *to,
            struct ConstValue value, struct Span span) {
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

  // Comptime → concrete numeric: range-check via fits_in.
  if (from->kind == TY_COMPTIME_INT) {
    if (!type_is_int(to)) {
      emit_coerce_error(s, span, from, to, NULL);
      return false;
    }
    // `to` is comptime_int — handled by from==to above when interned.
    if (to->kind == TY_COMPTIME_INT) return true;

    if (value.kind == CONST_INT) {
      const char *lo = NULL, *hi = NULL;
      if (fits_in(value, to, &lo, &hi)) return true;
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
      emit_coerce_error(s, span, from, to, NULL);
      return false;
    }
    if (value.kind == CONST_FLOAT) {
      const char *lo = NULL, *hi = NULL;
      if (fits_in(value, to, &lo, &hi)) return true;
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
      return false;
    }
    return true;
  }

  // Concrete-to-concrete: exact match only, no implicit widening.
  // Different bit-width or signedness requires an explicit cast at
  // the call site. This is intentional — silent widening hides
  // overflow paths.
  emit_coerce_error(s, span, from, to, NULL);
  return false;
}
