// tv_inspect.h — TypedValue inspection helpers. Phase 6 Batch 1.
//
// These read interned IPK_*_VALUE / reserved bool indices via ip_tag +
// ip_key. They REPLACE the const_eval.c trio of db_const_value_fits_in,
// db_const_value_to_str, const_val_eq + the const_in_range helper that
// previously consumed ConstValue. The new world stores values exclusively
// as interned IpIndexes; these helpers decode and operate.
//
// All helpers accept IP_NONE / error values gracefully (return false /
// write "?").
#ifndef ORE_DB_QUERY_TV_INSPECT_H
#define ORE_DB_QUERY_TV_INSPECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../db.h"
#include "../intern_pool/intern_pool.h"
#include "type_layer.h"  // SemaCtx for tv_value_in_range

// Decoded-scalar equality. CRITICAL property: `5 : comptime_int` and
// `5 : u32` intern to different IpIndex (different IPK_INT_VALUE.type) but
// compare equal here — pattern matching across mixed integer types depends
// on this. Supported value tags:
//   IPK_INT_VALUE  — pairwise on .value (signed/unsigned aware via .type)
//   IPK_FLOAT_VALUE — bit-pattern equality (both sourced from same intern)
//   IP_BOOL_TRUE/FALSE (reserved indices, tag IP_TAG_RESERVED_VALUE) —
//     identity compare
//   IPK_NAMESPACE_VALUE — on nsid.idx
//   IPK_ENUM_VARIANT_VALUE — on (enum_def.idx, variant_idx)
//   IPK_FN_VALUE — on def.idx (function reference identity)
// IP_NONE / mismatched tags → false.
bool tv_value_semantic_eq(struct db *s, IpIndex a, IpIndex b);

// Range check: does `value` fit in the numeric range of `target_type`?
// On false, optionally writes the type's range as static C string literals
// (caller does NOT free) to *out_lo / *out_hi for diag rendering. Replaces
// db_const_value_fits_in.
bool tv_fits_in(struct db *s, IpIndex value, IpIndex target_type,
                const char **out_lo, const char **out_hi);

// Format `value` for a diag ("1024" / "3.14" / "true"). Writes into buf
// (caller-owned, ≥32 bytes recommended). Returns buf. Replaces
// db_const_value_to_str.
const char *tv_value_to_str(struct db *s, IpIndex value, char *buf,
                            size_t buflen);

// Range-pattern membership: does `scrut_value` (IPK_INT_VALUE) fall inside
// the range expression `range_node` (a SK_BIN_EXPR with op SK_DOT_DOT_LT or
// SK_DOT_DOT_EQ)? Internally calls eval_expr on lo/hi and decodes.
// Returns false if either bound doesn't fold to a comptime int.
// Replaces const_in_range.
bool tv_value_in_range(const SemaCtx *ctx, SyntaxNode *range_node,
                       IpIndex scrut_value);

// Ordered scalar compare for SK_LT/LE/GT/GE. Decodes signedness from
// `.type` (u-types use unsigned compare; i-types signed; comptime_int
// adopts the typed operand's signedness when mixed, defaulting to signed
// when both are comptime). Returns IP_BOOL_TRUE / IP_BOOL_FALSE or
// IP_NONE if either operand isn't a foldable scalar.
IpIndex tv_int_compare(struct db *s, IpIndex l, IpIndex r, SyntaxKind opk);

#endif // ORE_DB_QUERY_TV_INSPECT_H
