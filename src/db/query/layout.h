// layout.h — type-layout computation. Pure, query-free; extracted from
// const_eval.c in Phase 4+5 because layout is NOT const-folding — it's
// consumed by @sizeOf / @alignOf (eval_expr) AND by coercion's
// range-check (tv_fits_in / db_const_value_fits_in).
//
// is_known=false on cycle / unsupported. Cycle detection is per-call
// (no memoization).
#ifndef ORE_DB_QUERY_LAYOUT_H
#define ORE_DB_QUERY_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

#include "../db.h"
#include "../intern_pool/intern_pool.h"

typedef struct {
  uint64_t size;
  uint64_t align;
  bool     is_known;
} OreLayout;

// Compute size + alignment of `t`. is_known=false on cycle / unsupported.
OreLayout db_layout_of_type(struct db *s, IpIndex t);

// Integer-range range-check helpers. Used by db_const_value_fits_in today
// and tv_fits_in in Phase 4+5. On false, *lo / *hi receive the type's
// range as static C string literals (caller doesn't free).
bool int_fits_signed(int64_t v, int bits, const char **lo, const char **hi);
bool int_fits_unsigned(int64_t v, int bits, const char **lo, const char **hi);
bool int_fits_unsigned_u64(uint64_t u, int bits, const char **lo,
                           const char **hi);

#endif // ORE_DB_QUERY_LAYOUT_H
