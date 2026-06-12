// layout.c — type-layout computation. Pure, query-free. Extracted from
// const_eval.c in Phase 4+5: layout is not const-folding; its callers are
// @sizeOf / @alignOf (eval_expr) and coercion fits-in (tv_fits_in /
// db_const_value_fits_in). See layout.h.

#include "layout.h"

#include "../db.h"
#include "../intern_pool/intern_pool.h"

#include <stdint.h>

// =====================================================================
// Integer range-check helpers — used by db_const_value_fits_in /
// tv_fits_in for diag-bearing range checks. Range strings are static
// C literals (no allocation; caller must NOT free).
// =====================================================================

bool int_fits_signed(int64_t v, int bits, const char **lo, const char **hi) {
  switch (bits) {
  case 8:  *lo = "-128"; *hi = "127";
           return v >= INT8_MIN && v <= INT8_MAX;
  case 16: *lo = "-32768"; *hi = "32767";
           return v >= INT16_MIN && v <= INT16_MAX;
  case 32: *lo = "-2147483648"; *hi = "2147483647";
           return v >= INT32_MIN && v <= INT32_MAX;
  case 64: *lo = "-9223372036854775808"; *hi = "9223372036854775807";
           return true;
  }
  return false;
}

bool int_fits_unsigned(int64_t v, int bits, const char **lo, const char **hi) {
  *lo = "0";
  if (v < 0) {
    switch (bits) {
    case 8:  *hi = "255"; break;
    case 16: *hi = "65535"; break;
    case 32: *hi = "4294967295"; break;
    case 64: *hi = "18446744073709551615"; break;
    }
    return false;
  }
  uint64_t u = (uint64_t)v;
  switch (bits) {
  case 8:  *hi = "255";                  return u <= UINT8_MAX;
  case 16: *hi = "65535";                return u <= UINT16_MAX;
  case 32: *hi = "4294967295";           return u <= UINT32_MAX;
  case 64: *hi = "18446744073709551615"; return true;
  }
  return false;
}

// J2: caller already has the uint64_t view (from an is_unsigned source
// where the int64_t bit-pattern would mis-read as negative). Skips the
// v<0 reject. Same range strings.
bool int_fits_unsigned_u64(uint64_t u, int bits, const char **lo,
                           const char **hi) {
  *lo = "0";
  switch (bits) {
  case 8:  *hi = "255";                  return u <= UINT8_MAX;
  case 16: *hi = "65535";                return u <= UINT16_MAX;
  case 32: *hi = "4294967295";           return u <= UINT32_MAX;
  case 64: *hi = "18446744073709551615"; return true;
  }
  return false;
}

// =====================================================================
// Cycle stack — guards struct/union recursion when a field type refers
// transitively back to the enclosing aggregate. is_known=false on cycle
// or stack-depth exhaustion.
// =====================================================================

#define LAYOUT_CYCLE_STACK_MAX 32

typedef struct {
  uint32_t depth;
  uint32_t ids[LAYOUT_CYCLE_STACK_MAX];
} LayoutCycle;

static OreLayout primitive_layout(IpIndex t) {
  OreLayout L = {0};
  L.is_known = true;
  // From ip_primitives.def — size and align columns.
  if      (t.v == IP_BOOL_TYPE.v) { L.size = 1;  L.align = 1; }
  else if (t.v == IP_U8_TYPE.v  || t.v == IP_I8_TYPE.v)  { L.size = 1; L.align = 1; }
  else if (t.v == IP_U16_TYPE.v || t.v == IP_I16_TYPE.v) { L.size = 2; L.align = 2; }
  else if (t.v == IP_U32_TYPE.v || t.v == IP_I32_TYPE.v) { L.size = 4; L.align = 4; }
  else if (t.v == IP_U64_TYPE.v || t.v == IP_I64_TYPE.v) { L.size = 8; L.align = 8; }
  else if (t.v == IP_F32_TYPE.v) { L.size = 4; L.align = 4; }
  else if (t.v == IP_F64_TYPE.v) { L.size = 8; L.align = 8; }
  else if (t.v == IP_USIZE_TYPE.v || t.v == IP_ISIZE_TYPE.v) {
    L.size = sizeof(void *); L.align = _Alignof(void *);
  } else {
    L.is_known = false;
  }
  return L;
}

static OreLayout layout_recurse(struct db *s, IpIndex t, LayoutCycle *cyc);

static OreLayout layout_struct_or_union(struct db *s, DefId def,
                                        LayoutCycle *cyc, bool is_union) {
  OreLayout L = {0};
  // Cycle check.
  for (uint32_t i = 0; i < cyc->depth; i++) {
    if (cyc->ids[i] == def.idx)
      return L; // is_known = false (cycle)
  }
  if (cyc->depth >= LAYOUT_CYCLE_STACK_MAX)
    return L; // depth exhausted — conservative miss
  cyc->ids[cyc->depth++] = def.idx;

  L.is_known = true;
  L.size = 0;
  L.align = 1;
  uint32_t n = db_aggregate_field_count(s, def);
  for (uint32_t i = 0; i < n; i++) {
    AggregateFieldEntry f = db_aggregate_field_at(s, def, i);
    OreLayout fl = layout_recurse(s, f.type, cyc);
    if (!fl.is_known) {
      L.is_known = false;
      cyc->depth--;
      return L;
    }
    if (fl.align > L.align) L.align = fl.align;
    if (is_union) {
      if (fl.size > L.size) L.size = fl.size;
    } else {
      // Pad to field alignment.
      if (fl.align > 0)
        L.size = (L.size + fl.align - 1) & ~(uint64_t)(fl.align - 1);
      L.size += fl.size;
    }
  }
  // Tail pad to overall align.
  if (L.align > 0)
    L.size = (L.size + L.align - 1) & ~(uint64_t)(L.align - 1);
  cyc->depth--;
  return L;
}

static OreLayout layout_recurse(struct db *s, IpIndex t, LayoutCycle *cyc) {
  OreLayout L = {0};
  if (!ip_index_is_valid(t))
    return L;

  IpTag tag = ip_tag(&s->intern, t);
  switch (tag) {
  case IP_TAG_PRIMITIVE_TYPE:
    return primitive_layout(t);

  case IP_TAG_PTR_TYPE:
  case IP_TAG_PTR_CONST_TYPE:
  case IP_TAG_MANY_PTR_TYPE:
  case IP_TAG_MANY_PTR_CONST_TYPE:
    L.is_known = true;
    L.size = sizeof(void *);
    L.align = _Alignof(void *);
    return L;

  case IP_TAG_SLICE_TYPE:
  case IP_TAG_SLICE_CONST_TYPE:
    // Slice = (ptr, len). 2 words.
    L.is_known = true;
    L.size = 2 * sizeof(void *);
    L.align = _Alignof(void *);
    return L;

  case IP_TAG_ARRAY_TYPE: {
    IpKey k = ip_key(&s->intern, t);
    OreLayout elem = layout_recurse(s, k.array_type.elem, cyc);
    if (!elem.is_known)
      return L; // cycle in element
    L.is_known = true;
    L.size = elem.size * k.array_type.size;
    L.align = elem.align;
    return L;
  }

  case IP_TAG_OPTIONAL_TYPE: {
    IpKey k = ip_key(&s->intern, t);
    OreLayout inner = layout_recurse(s, k.optional_type.elem, cyc);
    if (!inner.is_known)
      return L;
    L.is_known = true;
    // Conservative: tag byte + inner, aligned to inner.align.
    uint64_t size = inner.size + 1;
    if (inner.align > 0)
      size = (size + inner.align - 1) & ~(uint64_t)(inner.align - 1);
    L.size = size;
    L.align = inner.align > 0 ? inner.align : 1;
    return L;
  }

  case IP_TAG_FN_TYPE:
    // Function values are fn-pointer sized.
    L.is_known = true;
    L.size = sizeof(void *);
    L.align = _Alignof(void *);
    return L;

  case IP_TAG_STRUCT_TYPE: {
    IpKey k = ip_key(&s->intern, t);
    DefId def = (DefId){.idx = k.struct_type.zir_node_id};
    bool is_union = (db_def_kind(s, def) == KIND_UNION);
    return layout_struct_or_union(s, def, cyc, is_union);
  }

  case IP_TAG_ENUM_TYPE: {
    // Simple v0: enum tag is a u32 (4 bytes). Tightening to the
    // minimum-width-fitting-all-variants is sema_legacy behavior
    // worth porting later; for the F1 fixture this is sufficient.
    L.is_known = true;
    L.size = 4;
    L.align = 4;
    return L;
  }

  default:
    return L; // is_known = false
  }
}

OreLayout db_layout_of_type(struct db *s, IpIndex t) {
  LayoutCycle cyc = {0};
  return layout_recurse(s, t, &cyc);
}
