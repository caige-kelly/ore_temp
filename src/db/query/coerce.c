// Coerce — Phase H. See coerce.h for the surface + invariants.
//
// Implementation note: the structural rules below are a 1:1 port of the
// can_coerce body that lived in infer.c through Phase F1 (and which was
// itself a 1:1 port of sema_legacy/typechecker/coerce.c at 2938187),
// plus H1.5's concrete-int width-change rules (mirroring Zig Sema.zig
// lines 28897-28905 widening + 29618-29629 narrow-with-range).

#include "coerce.h"

#include "const_eval.h"
#include "../diag/diag.h"

#include <stddef.h>
#include <stdint.h>

// --- Structural-only sub-check (pure, recursive) --------------------------
//
// Used internally by `coerce()` for the optional-lift recursion. Returns
// true/false on shape match; does not see SyntaxNode or emit diags. Kept
// distinct from the public entrypoint so a maintainer doesn't confuse it
// with an in-memory-equivalence probe.

static bool coerce_structural(struct db *s, IpIndex actual, IpIndex expected) {
  // Cascade-suppression sentinel — IP_NONE is ore's "poisoned type."
  if (actual.v == IP_NONE.v || expected.v == IP_NONE.v)
    return true;
  if (actual.v == expected.v)
    return true;
  // Bottom: noreturn coerces to any type.
  if (actual.v == IP_NORETURN_TYPE.v)
    return true;

  IpTag at = ip_tag(&s->intern, actual);
  IpTag et = ip_tag(&s->intern, expected);

  // Pointer / slice / many-ptr variance: drop mut (X → const X), same elem.
  if ((at == IP_TAG_PTR_TYPE || at == IP_TAG_PTR_CONST_TYPE) &&
      (et == IP_TAG_PTR_TYPE || et == IP_TAG_PTR_CONST_TYPE)) {
    IpKey ak = ip_key(&s->intern, actual), ek = ip_key(&s->intern, expected);
    if (ak.ptr_type.elem.v == ek.ptr_type.elem.v &&
        (at != IP_TAG_PTR_CONST_TYPE || et == IP_TAG_PTR_CONST_TYPE))
      return true;
  }
  if ((at == IP_TAG_SLICE_TYPE || at == IP_TAG_SLICE_CONST_TYPE) &&
      (et == IP_TAG_SLICE_TYPE || et == IP_TAG_SLICE_CONST_TYPE)) {
    IpKey ak = ip_key(&s->intern, actual), ek = ip_key(&s->intern, expected);
    if (ak.slice_type.elem.v == ek.slice_type.elem.v &&
        (at != IP_TAG_SLICE_CONST_TYPE || et == IP_TAG_SLICE_CONST_TYPE))
      return true;
  }
  if ((at == IP_TAG_MANY_PTR_TYPE || at == IP_TAG_MANY_PTR_CONST_TYPE) &&
      (et == IP_TAG_MANY_PTR_TYPE || et == IP_TAG_MANY_PTR_CONST_TYPE)) {
    IpKey ak = ip_key(&s->intern, actual), ek = ip_key(&s->intern, expected);
    if (ak.many_ptr_type.elem.v == ek.many_ptr_type.elem.v &&
        (at != IP_TAG_MANY_PTR_CONST_TYPE ||
         et == IP_TAG_MANY_PTR_CONST_TYPE))
      return true;
  }
  // Array-ptr decay: ^[N]T → []T / [^]T (const flows).
  if (at == IP_TAG_PTR_TYPE || at == IP_TAG_PTR_CONST_TYPE) {
    IpKey ak = ip_key(&s->intern, actual);
    if (ip_tag(&s->intern, ak.ptr_type.elem) == IP_TAG_ARRAY_TYPE) {
      IpKey arrk = ip_key(&s->intern, ak.ptr_type.elem);
      bool a_const = (at == IP_TAG_PTR_CONST_TYPE);
      if (et == IP_TAG_SLICE_TYPE || et == IP_TAG_SLICE_CONST_TYPE) {
        IpKey ek = ip_key(&s->intern, expected);
        if (arrk.array_type.elem.v == ek.slice_type.elem.v &&
            (!a_const || et == IP_TAG_SLICE_CONST_TYPE))
          return true;
      }
      if (et == IP_TAG_MANY_PTR_TYPE || et == IP_TAG_MANY_PTR_CONST_TYPE) {
        IpKey ek = ip_key(&s->intern, expected);
        if (arrk.array_type.elem.v == ek.many_ptr_type.elem.v &&
            (!a_const || et == IP_TAG_MANY_PTR_CONST_TYPE))
          return true;
      }
    }
  }
  // nil → ?T / ^T / [^]T / []T  (+ all const variants)
  if (actual.v == IP_NIL_TYPE.v &&
      (et == IP_TAG_OPTIONAL_TYPE || et == IP_TAG_PTR_TYPE ||
       et == IP_TAG_PTR_CONST_TYPE || et == IP_TAG_MANY_PTR_TYPE ||
       et == IP_TAG_MANY_PTR_CONST_TYPE || et == IP_TAG_SLICE_TYPE ||
       et == IP_TAG_SLICE_CONST_TYPE))
    return true;
  // Optional lift: T → ?T  (recursive on the elem)
  if (et == IP_TAG_OPTIONAL_TYPE) {
    IpKey ek = ip_key(&s->intern, expected);
    if (coerce_structural(s, actual, ek.optional_type.elem))
      return true;
  }
  // Phase K — fn-ptr variance. Covariant return, contravariant params,
  // exact modifier match. ore admits fns as first-class values
  // (fn_in_type_position.ore), so this rule fires whenever a fn value
  // crosses an assignability boundary. The standard use case: passing
  // a `fn(^const T) void` where `fn(^T) void` is expected — the
  // contravariant param check unwraps the const-drop variance for us.
  if (at == IP_TAG_FN_TYPE && et == IP_TAG_FN_TYPE) {
    IpKey ak = ip_key(&s->intern, actual), ek = ip_key(&s->intern, expected);
    if (ak.fn_type.n_params == ek.fn_type.n_params &&
        ak.fn_type.modifiers == ek.fn_type.modifiers) {
      // Covariant return: actual.ret coerces TO expected.ret. (Caller
      // sees expected.ret; we may produce a more specific value.)
      if (coerce_structural(s, ak.fn_type.ret, ek.fn_type.ret)) {
        bool params_ok = true;
        for (size_t i = 0; i < ak.fn_type.n_params; i++) {
          // Contravariant: expected.params[i] coerces TO actual.params[i].
          // (Caller passes a value that satisfies expected.params[i]; our
          // fn must accept it via actual.params[i].)
          if (!coerce_structural(s, ek.fn_type.params[i],
                                 ak.fn_type.params[i])) {
            params_ok = false;
            break;
          }
        }
        if (params_ok)
          return true;
      }
    }
  }

  // Comptime numeric → concrete (range-check is the caller's job; here we
  // only say the shape matches).
  if (actual.v == IP_COMPTIME_INT_TYPE.v &&
      (is_concrete_int(expected) || is_concrete_float(expected) ||
       expected.v == IP_COMPTIME_FLOAT_TYPE.v))
    return true;
  if (actual.v == IP_COMPTIME_FLOAT_TYPE.v && is_concrete_float(expected))
    return true;

  // H1.5 — concrete-int width-change. Same-tag was caught by the
  // identity check above; here we admit widening that does NOT change
  // sign-interpretation OR signedness (Zig: small-unsigned → wider-
  // signed only). Narrowing requires a const value — handled in
  // `coerce()`, not here.
  if (is_concrete_int(actual) && is_concrete_int(expected)) {
    int af = int_bits(actual), ef = int_bits(expected);
    bool a_unsigned = is_unsigned_int(actual);
    bool a_signed = is_signed_int(actual);
    bool e_unsigned = is_unsigned_int(expected);
    bool e_signed = is_signed_int(expected);
    // Unsigned → unsigned wider: u8 → u16 etc.
    if (a_unsigned && e_unsigned && ef > af)
      return true;
    // Signed → signed wider: i8 → i16 etc.
    if (a_signed && e_signed && ef > af)
      return true;
    // Small-unsigned → strictly-wider-signed: u8 → i16/i32/i64. Zig's
    // rule — same-width unsigned → signed (`u32 → i32`) is rejected
    // because it loses representability for values ≥ 2^31.
    if (a_unsigned && e_signed && ef > af)
      return true;
  }
  return false;
}

// Unwrap any chain of `?` wrappers down to the underlying concrete type.
// `?u8` → `u8`, `??u8` → `u8`. For non-optional input, returns input. Used
// for range-checking — `let x: ?u8 = 1024` should still range-check 1024
// against u8's range, not against the optional shape.
static IpIndex unwrap_optional_chain(struct db *s, IpIndex t) {
  while (ip_tag(&s->intern, t) == IP_TAG_OPTIONAL_TYPE) {
    IpKey k = ip_key(&s->intern, t);
    t = k.optional_type.elem;
  }
  return t;
}

// --- Public entrypoint ----------------------------------------------------

Coercion coerce(const SemaCtx *ctx, SyntaxNode *node, IpIndex actual,
                IpIndex expected) {
  Coercion out = {COERCE_OK, NULL, NULL};

  // Structural-first. If shape matches we may still fail FAIL_RANGE
  // for comptime → concrete narrows AND for H1.5 const-narrowing
  // (u16 → u8 where the value fits).
  if (coerce_structural(ctx->s, actual, expected)) {
    // Unwrap `?T` chain for range purposes — `comptime_int → ?u8` must
    // range-check the literal against u8's range, not the optional.
    IpIndex range_target = unwrap_optional_chain(ctx->s, expected);
    bool comptime_narrow = (actual.v == IP_COMPTIME_INT_TYPE.v ||
                            actual.v == IP_COMPTIME_FLOAT_TYPE.v) &&
                           (is_concrete_int(range_target) ||
                            is_concrete_float(range_target));
    if (comptime_narrow && node) {
      ConstValue v = db_const_eval(ctx->s, ctx->file_local, node);
      if (v.kind != CONST_NONE) {
        const char *lo = NULL, *hi = NULL;
        if (!db_const_value_fits_in(ctx->s, v, range_target, &lo, &hi)) {
          out.kind = COERCE_FAIL_RANGE;
          out.range_lo = lo;
          out.range_hi = hi;
        }
      }
    }
    return out;
  }

  // H1.5 — concrete-int narrowing succeeds iff we have a const value
  // that fits in `expected`. e.g. `u16 → u8` with the source being a
  // literal `200` is OK; the same coerce on a runtime `u16` is
  // FAIL_TYPE ("use @intCast"). Structural already rejected this width
  // change, so we treat it as a const-only exception. Optional unwrap
  // applies symmetrically so `u16 → ?u8` works the same way.
  IpIndex range_target = unwrap_optional_chain(ctx->s, expected);
  if (is_concrete_int(actual) && is_concrete_int(range_target) && node) {
    ConstValue v = db_const_eval(ctx->s, ctx->file_local, node);
    if (v.kind == CONST_INT) {
      const char *lo = NULL, *hi = NULL;
      if (db_const_value_fits_in(ctx->s, v, range_target, &lo, &hi)) {
        return out; // COERCE_OK
      }
      out.kind = COERCE_FAIL_RANGE;
      out.range_lo = lo;
      out.range_hi = hi;
      return out;
    }
  }

  out.kind = COERCE_FAIL_TYPE;
  return out;
}

bool coerce_or_diag(const SemaCtx *ctx, SyntaxNode *node, IpIndex actual,
                    IpIndex expected) {
  Coercion c = coerce(ctx, node, actual, expected);
  if (c.kind == COERCE_OK)
    return true;

  DiagAnchor span = diag_anchor_of_node((uint16_t)ctx->file_local.idx, node);
  if (c.kind == COERCE_FAIL_TYPE) {
    db_emit(ctx->s, DIAG_ERROR, span,
            "expected type '%T', found '%T'", expected, actual);
    return false;
  }
  // FAIL_RANGE — H3 Zig parity strings, keep ore's (range LO..HI) extension.
  ConstValue v = node ? db_const_eval(ctx->s, ctx->file_local, node) : (ConstValue){0};
  char vbuf[64];
  db_const_value_to_str(v, vbuf, sizeof(vbuf));
  if (v.kind == CONST_FLOAT) {
    db_emit(ctx->s, DIAG_ERROR, span,
            "type '%T' cannot represent float value '%s'", expected, vbuf);
  } else if (c.range_lo && c.range_hi) {
    db_emit(ctx->s, DIAG_ERROR, span,
            "type '%T' cannot represent integer value '%s' (range %s..%s)",
            expected, vbuf, c.range_lo, c.range_hi);
  } else {
    db_emit(ctx->s, DIAG_ERROR, span,
            "type '%T' cannot represent integer value '%s'", expected, vbuf);
  }
  return false;
}
