#ifndef ORE_SEMA_TYPE_FITS_H
#define ORE_SEMA_TYPE_FITS_H

#include <stdbool.h>
#include <stdint.h>

#include "../eval/const_eval.h"

struct Type;

// Strict range check: does `v` fit in `t`'s range?
//
// Range only — precision loss within range is acceptable. Examples:
//   * `1.5e10` in f32: fits (< f32 max), even though precision is
//     lost (the round-trip value differs).
//   * `1.5e40` in f32: does NOT fit (> f32 max).
//   * `200` in u8: fits (< 255).
//   * `1_000_000_000` in u8: does NOT fit (> 255).
//   * `-100` in u8: does NOT fit (< 0).
//
// Returns false for type mismatches (int value into float type, etc.).
// Caller is responsible for emitting the diagnostic on a false return;
// `out_min_str` and `out_max_str` get set to literal range bounds for
// error messages (NULL if not relevant).
bool fits_in(struct ConstValue v, const struct Type *t,
             const char **out_min_str, const char **out_max_str);

// Convenience: format `v` for a diagnostic. Writes into `buf` (caller
// owns; must be at least 32 bytes). Returns `buf`.
const char *const_value_to_str(struct ConstValue v, char *buf, size_t buflen);

#endif
