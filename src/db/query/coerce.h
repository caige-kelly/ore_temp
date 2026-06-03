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

// --- Effects-4 row API ----------------------------------------------------
//
// Three helpers that infer.c (and type.c's KIND_EFFECT typing) need to talk
// to the same row-substitution table coerce_structural already uses.
//
//   row_union(ctx, a, b, node)
//     Merge two effect-row IpIndices into one canonical row under
//     ctx->row_subst. Pure ⊕ X = X; both row-vars binds one to the other;
//     two concrete rows merge their sorted label lists (duplicates per
//     Koka) and recursively unify the tails. Returns IP_NONE on
//     unification failure (e.g. two closed rows with conflicting labels).
//     `node` is forwarded to row_unify for cycle-diag anchoring; pass NULL
//     when no SyntaxNode is in scope (the cycle diag still emits but anchors
//     at file start as a coarse fallback).
//
//   row_unify(ctx, a, b, node)
//     Unify-as-equation entry point (public name for the previously-
//     static unify_effect_rows). Used by SK_HANDLE_EXPR's discharge step
//     (`unify action_row ≡ ⟨targeted | μ_residual⟩`) and by
//     db_query_infer_body's body-vs-declared row gate. `node` lets the
//     cycle diag anchor via the active decl's wrapper map (DIAG_ANCHOR_BODY,
//     drift-stable across sibling reparses); NULL falls back to file-start.
//
//   row_resolve(ctx, r)
//     Read a row through the substitution chain. Lets a caller see what
//     a row variable bound to after a prior unification step.
//
//   row_intern(s, labels, n_labels, tail)
//     Plain interning helper — sorted labels (duplicates allowed) + a
//     resolved tail (IP_EMPTY_EFFECT_ROW or a row-var IpIndex).
IpIndex row_union  (const SemaCtx *ctx, IpIndex a, IpIndex b,
                    const SyntaxNode *node);
bool    row_unify  (const SemaCtx *ctx, IpIndex a, IpIndex b,
                    const SyntaxNode *node);
IpIndex row_resolve(const SemaCtx *ctx, IpIndex r);
IpIndex row_intern (struct db *s, const DefId *labels, size_t n_labels,
                    IpIndex tail);
// Resolve + splice in any bound EFFECT_ROW tails, returning a canonical
// IpIndex whose tail is either IP_EMPTY_EFFECT_ROW or an unbound row var.
// Use before passing a row to the diag formatter so user-facing output
// reflects the post-substitution shape.
IpIndex row_flatten(const SemaCtx *ctx, IpIndex r);

// Effects-4.5a — call-site instantiation. Given a fn_type IpIndex
// whose signature contains row variables (either directly in the fn's
// effect_row or nested in param/return fn types), mint one fresh row
// var per DISTINCT old row var, substitute everywhere, and re-intern.
// Returns a new fn_type IpIndex; the input is left untouched.
//
// Two occurrences of the SAME old row var map to the SAME fresh one
// (preserves the `<..e>`-shared-by-param-and-return name scope).
// Non-fn-type inputs are returned unchanged; only IPK_FN_TYPE inputs
// trigger a walk.
//
// Does NOT touch ctx->row_subst — that's the per-frame unification
// state. Each call-site instantiation is independent.
IpIndex instantiate_fn_for_call_site(struct db *s, IpIndex fn_ty);

// Effects-4.5c — defaulting pass. Walk a row and bind any unbound
// row-var tail (after row_resolve chase) to IP_EMPTY_EFFECT_ROW in
// ctx->row_subst. After this, row_flatten/row_resolve on the same
// IpIndex returns a closed row. Used at end of INFER_BODY to stop
// raw `<..rv#N>` from leaking into user-facing diags.
void ground_unbound_row_vars(const SemaCtx *ctx, IpIndex r);

// --- Type-default-value query (Array-init §A) ----------------------------
//
// Returns a non-IP_NONE IpIndex iff `type` has a canonical default value
// — a "safest zero-state" that {} init-shorthand uses to populate
// elements without listing them. Returns IP_NONE for types where no
// default makes sense (raw pointers under the strict-nil model, fn
// types, structs without per-field defaults, enums without a default
// variant).
//
// The returned value is sema-level: callers use it as a presence/
// absence signal, not as a concrete bit pattern. Codegen (when it
// lands) will read the TYPE to emit the actual init instructions.
//
// Phase-1 supported cases:
//   - Numeric primitives (int / float / comptime_int / comptime_float)
//     → IP_ZERO_USIZE (marker — codegen reads the real type)
//   - IP_BOOL_TYPE → IP_BOOL_FALSE
//   - IPK_OPTIONAL_TYPE → IP_NIL_TYPE
//   - IPK_ARRAY_TYPE → recursive: defaults to element-wise default
//     (returns non-IP_NONE iff the element type itself has a default)
//
// Not supported (returns IP_NONE):
//   - IPK_PTR_TYPE, IPK_MANY_PTR_TYPE, IPK_SLICE_TYPE — strict-nil
//     model: raw pointers / slices / many-ptrs are non-null; use the
//     `?` wrapper if you need a nullable default.
//   - IPK_STRUCT_TYPE, IPK_ENUM_TYPE — defer to a future per-field /
//     @default(Variant) syntax.
//   - IPK_FN_TYPE, IPK_EFFECT_TYPE, IPK_HANDLER_TYPE — no default.
IpIndex ip_default_value(struct db *s, IpIndex type);

#endif // ORE_DB_QUERY_COERCE_H
