#ifndef ORE_DB_QUERY_CONST_EVAL_H
#define ORE_DB_QUERY_CONST_EVAL_H

// Comptime const-folding + layout — F1 port of sema_legacy's
// `comptime/const_eval` + `typechecker/{layout,fits}`. Pure
// (non-memoized) functions for now; if profiling shows hotness, wire
// to a DB_QUERY_GUARD slot keyed on SyntaxNodePtr.
//
// What's covered (F1 subset):
//   - Literals (int / float / bool).
//   - Bin arith (+ - * / %).
//   - Prefix unary (- ! ~).
//   - SK_REF_EXPR → resolve to top-level `::` bind and recurse
//     (chain folding: `MAX :: 1024; HALF :: MAX / 2`).
//   - SK_BUILTIN_EXPR for @sizeOf / @alignOf (delegates to layout).
//
// F2 will add: if/switch with comptime cond, blocks with const tail.
//
// Range-check: db_const_value_fits_in mirrors sema_legacy/fits.c.
// Used by infer.c at the comptime → concrete coerce site to fire
// "value 1024 does not fit in u8 (range 0..255)" diags.
//
// Layout: db_layout_of_type recurses through pointer/slice/array/
// struct/enum, computing size + alignment using ip_primitives.def's
// SIZE/ALIGN columns for primitives. Struct layout uses C-style
// rules. By-value cycles (struct containing itself by value) return
// is_known=false; the caller emits the diag.

#include <stdbool.h>
#include <stdint.h>

#include "../db.h"
#include "../intern_pool/intern_pool.h"
#include "../../syntax/syntax.h"

typedef enum {
  CONST_NONE,
  CONST_INT,
  CONST_FLOAT,
  CONST_BOOL,
  // #3 — comptime carrier for synthetic builtin namespaces (@target,
  // @build). Returned by SK_BUILTIN_EXPR(target|build) and consumed by
  // SK_FIELD_EXPR's namespace-base arm.
  CONST_NAMESPACE,
  // #3 — comptime carrier for a specific enum variant. Returned by
  // SK_ENUM_REF_EXPR (pattern `.macos`) and SK_FIELD_EXPR's namespace-
  // member lookup when the member is itself an enum-variant-typed
  // constant (`@target.os` → CONST_ENUM_VARIANT for the host's os).
  // Compared via const_values_equal for switch-arm pattern matching.
  CONST_ENUM_VARIANT,
  // First-class comptime type value (Zig-style unification): a value whose
  // TYPE is `type` and whose VALUE is a concrete type. The payload is just the
  // type's IpIndex — no separate reification. Produced by a type NAME in value
  // position (`u32`), by a `t: type` param in value position, and consumed by
  // resolve_type_expr to turn a type-valued local (`c : type = u32`) back into
  // a usable type in type position.
  CONST_TYPE,
} ConstKind;

typedef struct {
  ConstKind kind;
  // J2: CONST_INT only. Selects between (uint64_t)int_val and (int64_t)
  // int_val for fits-in / printing / arithmetic. Set true at literal
  // parse for values > INT64_MAX (which would otherwise be stored as
  // negative int64_t). Cleared by unary minus (negation produces signed
  // semantics). Bin-ops where the high bit matters (div, mod, shr)
  // consult this; bin-ops that are bit-identical for signed/unsigned
  // (add, sub, mul, shl) propagate it (unsigned if either operand is).
  bool      is_unsigned;
  union {
    int64_t int_val;
    double  float_val;
    bool    bool_val;
    NamespaceId nsid;             // CONST_NAMESPACE
    struct {
      DefId    enum_def;          // CONST_ENUM_VARIANT
      uint32_t variant_idx;       // index into db_enum_variants(enum_def)
    } enum_variant;
    IpIndex  type_val;            // CONST_TYPE — the type's IpIndex
  };
} ConstValue;

// Diag-anchor handle for const_eval's emit sites. const_eval has no
// SemaCtx of its own, but callers in SemaCtx-bearing frames (infer.c,
// coerce.c, type.c) can pass their active decl's wrapper map so emitted
// cycle / depth / effectful-call diags structurally re-anchor across
// sibling-prepend edits instead of drifting via frozen FILE_RAW offsets.
// `decl_ast_map == NULL` falls back to the existing `diag_anchor_of_node`
// behavior (byte-frozen but at least token-relative).
struct DeclAstIdMap;
struct NodeTypeBuilder;
typedef struct ConstDiagAnchorCtx {
  const struct DeclAstIdMap *decl_ast_map;
  uint32_t                   decl_key;
  // Live frame context for resolving body-local type params (`t: type`) when
  // const-eval resolves a type-position arg (@sizeOf(t)/@alignOf(t)) inside a
  // monomorphized instance body. All NONE/NULL outside a fn frame — the
  // no-frame callers ({0} / decl_ast_map-only literals) get safe defaults, and
  // resolution then falls back to the top-level type-name lookup as before.
  DefId                      enclosing_fn;
  HashMap                   *type_subst;
  struct NodeTypeBuilder    *types;
} ConstDiagAnchorCtx;

// Try to const-evaluate `node`. Returns CONST_NONE for any non-foldable
// shape (runtime expr, unresolved ref, partial type, etc.). `fid` is
// the home file of the expression being evaluated — needed so cross-
// file ref chains (where the referenced binding lives in a different
// file of the same namespace) correctly reach the right green root for
// type-position lookups (J3) and so diag spans attribute to the right
// file. Derive the namespace via `db_get_file_namespace(s, fid)` when
// needed inside an evaluator branch.
ConstValue db_const_eval(struct db *s, FileId fid, SyntaxNode *node,
                         ConstDiagAnchorCtx anchor);

// Variant of db_const_eval that resolves a bare `.variant` SK_ENUM_REF_EXPR
// against `enum_ctx` — needed for switch-arm pattern folding when the
// scrutinee folded to CONST_ENUM_VARIANT and the arm patterns are bare
// `.variant` references. Returns CONST_NONE for any non-bare-enum-ref
// shape (delegates to db_const_eval).
ConstValue db_const_eval_with_enum_ctx(struct db *s, FileId fid,
                                       SyntaxNode *node, DefId enum_ctx,
                                       ConstDiagAnchorCtx anchor);

// Range-check: does v fit in the numeric range of target type t?
//   - CONST_INT into a concrete int: bounds check via int_fits_*.
//   - CONST_FLOAT into f32/f64: magnitude check.
//   - CONST_INT into comptime_int / CONST_FLOAT into comptime_float: true.
//   - Type mismatch (int into float, etc.): false.
// On false, optionally writes the type's range as string literals
// (caller doesn't free) to *out_lo / *out_hi for diag rendering.
bool db_const_value_fits_in(struct db *s, ConstValue v, IpIndex t,
                            const char **out_lo, const char **out_hi);

// Format v for a diagnostic ("1024" / "3.14" / "true"). Writes into
// buf (caller-owned, ≥32 bytes). Returns buf.
const char *db_const_value_to_str(ConstValue v, char *buf, size_t buflen);

// Layout (OreLayout, db_layout_of_type, int_fits_*) — moved to layout.h
// in Phase 4+5. Include "layout.h" directly; it is not const-folding.

#endif // ORE_DB_QUERY_CONST_EVAL_H
