#ifndef ORE_DB_QUERY_COERCE_H
#define ORE_DB_QUERY_COERCE_H

// Coerce — unified type-coercion entrypoint (Phase H).
//
// Single source of truth for "does `actual` satisfy `expected`, and if
// not, what failed?" Folds the F1-era structural check
// (`can_coerce`) + range-check (`coerce_range_check_or_diag`) into one
// outcome so a caller cannot accept structurally and forget to
// range-check a comptime → concrete narrow.
//
// === Invariants ===========================================================
//
// 1. `coerce()` does NOT modify NodeTypesRange. Optional-lift (T → ?T),
//    nil-lift, array-ptr decay (^[N]T → []T / [^]T) all return OK without
//    restamping — the node keeps its natural type. Rationale: hover on
//    `42` in `var x: ?i32 = 42` should show `i32`, not `?i32` (the
//    destination is recoverable from the parent decl). The single
//    restamping case is comptime-literal narrowing, handled at the
//    check_expr leaf site in infer.c (NOT in coerce).
//
// 2. ore types are canonical — IpIndex equality implies structural
//    equality. This is why ore ships the `coerce` flavor only and skips
//    Zig's `coerceInMemoryAllowed(dest_is_mut=true)` distinction. When
//    nominal types or through-pointer mutation land, add an IM flavor as
//    a sibling function; the existing rules don't move.
//
// 3. Recursive optional-lift inside `coerce()` uses an internal
//    `coerce_structural` helper (not the public entrypoint). Naming
//    keeps its role explicit so future readers don't mistake it for an
//    in-memory probe.
//
// === Queued rule gaps (Zig parity, future) =================================
//
//   - Error unions (E!T) — when `error` types land.
//   - Function-pointer variance (callconv, contravariant params) —
//     when fn-as-value lands.
//   - Tuples / vectors / enum-literal → tagged union — when each
//     corresponding feature lands.
//   - `^T → ^[1]T`, sentinel-terminated `[N:s]T` / `[*:s]T` — same.
//   - Arbitrary-bit ints (u1..u65535) — Phase I in the plan file. The
//     rule table is parametric over "is this an int" / "what's the elem
//     type" — Phase I adds an int-tag branch to the structural check;
//     no new rows in the table.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../db.h"
#include "../intern_pool/intern_pool.h"
#include "type_layer.h"
#include "../../syntax/syntax.h"

// --- Numeric predicates (shared with infer.c and const_eval.c) ------------
//
// Reserved int/float primitives live at IpIndex.v < 32, so each predicate
// is a constant-mask shift. When Phase I lands wider ints, the bitmask
// stays as the hot-path cache for the reserved widths; an `ip_tag(t) ==
// IP_TAG_INT_TYPE` branch handles the compound case.

#define ORE_IP_BIT(name) (1u << IP_INDEX_##name##_TYPE)

#define ORE_CONCRETE_INT_MASK                                                  \
  (ORE_IP_BIT(U8) | ORE_IP_BIT(I8) | ORE_IP_BIT(U16) | ORE_IP_BIT(I16) |       \
   ORE_IP_BIT(U32) | ORE_IP_BIT(I32) | ORE_IP_BIT(U64) | ORE_IP_BIT(I64) |     \
   ORE_IP_BIT(USIZE) | ORE_IP_BIT(ISIZE))
#define ORE_CONCRETE_FLOAT_MASK (ORE_IP_BIT(F32) | ORE_IP_BIT(F64))
#define ORE_COMPTIME_NUMERIC_MASK                                              \
  (ORE_IP_BIT(COMPTIME_INT) | ORE_IP_BIT(COMPTIME_FLOAT))
#define ORE_NUMERIC_MASK                                                       \
  (ORE_CONCRETE_INT_MASK | ORE_CONCRETE_FLOAT_MASK | ORE_COMPTIME_NUMERIC_MASK)

#define ORE_SIGNED_INT_MASK                                                    \
  (ORE_IP_BIT(I8) | ORE_IP_BIT(I16) | ORE_IP_BIT(I32) | ORE_IP_BIT(I64) |      \
   ORE_IP_BIT(ISIZE))
#define ORE_UNSIGNED_INT_MASK                                                  \
  (ORE_IP_BIT(U8) | ORE_IP_BIT(U16) | ORE_IP_BIT(U32) | ORE_IP_BIT(U64) |      \
   ORE_IP_BIT(USIZE))

static inline bool is_concrete_int(IpIndex t) {
  return t.v < 32u && ((ORE_CONCRETE_INT_MASK >> t.v) & 1u);
}
static inline bool is_concrete_float(IpIndex t) {
  return t.v < 32u && ((ORE_CONCRETE_FLOAT_MASK >> t.v) & 1u);
}
static inline bool is_numeric(IpIndex t) {
  return t.v < 32u && ((ORE_NUMERIC_MASK >> t.v) & 1u);
}
static inline bool is_signed_int(IpIndex t) {
  return t.v < 32u && ((ORE_SIGNED_INT_MASK >> t.v) & 1u);
}
static inline bool is_unsigned_int(IpIndex t) {
  return t.v < 32u && ((ORE_UNSIGNED_INT_MASK >> t.v) & 1u);
}

// Bit-width of a concrete int type (0 for non-ints). usize/isize report
// the host pointer width — when target-cross-compile lands this consults
// the target table instead.
static inline int int_bits(IpIndex t) {
  if (t.v == IP_U8_TYPE.v || t.v == IP_I8_TYPE.v)
    return 8;
  if (t.v == IP_U16_TYPE.v || t.v == IP_I16_TYPE.v)
    return 16;
  if (t.v == IP_U32_TYPE.v || t.v == IP_I32_TYPE.v)
    return 32;
  if (t.v == IP_U64_TYPE.v || t.v == IP_I64_TYPE.v)
    return 64;
  if (t.v == IP_USIZE_TYPE.v || t.v == IP_ISIZE_TYPE.v)
    return (int)(sizeof(void *) * 8);
  return 0;
}

// --- Coerce API -----------------------------------------------------------

typedef enum {
  COERCE_OK,         // structurally fits AND (if applicable) range-fits
  COERCE_FAIL_TYPE,  // structural mismatch
  COERCE_FAIL_RANGE, // structural fit, value out of range
} CoerceKind;

typedef struct {
  CoerceKind kind;
  // For FAIL_RANGE only — the OOR bounds for diag rendering. Static
  // string literals owned by const_eval; callers don't free. NULL when
  // the source value isn't a const (FAIL_RANGE then degenerates to a
  // "not representable" diag rather than a range one).
  const char *range_lo;
  const char *range_hi;
} Coercion;

// Source of truth. node may be NULL — callers without a SyntaxNode get
// the structural-only outcome (range-check needs the node to drive
// db_const_eval, so without it FAIL_RANGE collapses to OK).
Coercion coerce(const SemaCtx *ctx, SyntaxNode *node, IpIndex actual,
                IpIndex expected);

// Convenience: coerce + emit Zig-parity diag on failure. Returns true on
// OK. Use this at every site that today emits "expected type '%T', found
// '%T'" or "type '%T' cannot represent ..." by hand.
bool coerce_or_diag(const SemaCtx *ctx, SyntaxNode *node, IpIndex actual,
                    IpIndex expected);

#endif // ORE_DB_QUERY_COERCE_H
