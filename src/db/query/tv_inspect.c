// tv_inspect.c — see tv_inspect.h. Phase 6 Batch 1.

#include "tv_inspect.h"

#include "coerce.h"   // int_bits, is_signed_int, is_unsigned_int
#include "eval.h"     // eval_expr (for tv_value_in_range)
#include "layout.h"   // int_fits_signed / _unsigned / _unsigned_u64
#include "../../ast/ast_expr.h"   // BinExpr_cast, BinExpr_op_kind, etc.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// -------------------------------------------------------------------
// Helpers — minimal decode wrappers.
// -------------------------------------------------------------------

// True iff `t` is one of the unsigned concrete int types (u8/u16/u32/u64/usize).
static bool tv_type_is_unsigned(IpIndex t) {
  return is_unsigned_int(t);
}

// Decode a value IpIndex into (kind, int64_t bits, double bits) discriminator.
// Returns true on a recognized scalar value; out fields populated.
typedef enum {
  TV_KIND_NONE = 0,
  TV_KIND_INT,
  TV_KIND_FLOAT,
  TV_KIND_BOOL,
  TV_KIND_NAMESPACE,
  TV_KIND_ENUM_VARIANT,
} TvKind;

typedef struct {
  TvKind   kind;
  IpIndex  type;          // .type half of IPK_INT_VALUE / IPK_FLOAT_VALUE
  int64_t  int_val;
  double   float_val;
  bool     bool_val;      // for TV_KIND_BOOL
  uint32_t nsid;          // for TV_KIND_NAMESPACE
  DefId    enum_def;      // for TV_KIND_ENUM_VARIANT
  uint32_t variant_idx;
} TvDecoded;

static TvDecoded tv_decode(struct db *s, IpIndex v) {
  TvDecoded out = {0};
  if (v.v == IP_NONE.v || ip_is_error(v))
    return out;
  // Reserved-index bool: IP_BOOL_TRUE / IP_BOOL_FALSE share IP_TAG_RESERVED_VALUE.
  if (v.v == IP_BOOL_TRUE.v) {
    out.kind = TV_KIND_BOOL;
    out.bool_val = true;
    return out;
  }
  if (v.v == IP_BOOL_FALSE.v) {
    out.kind = TV_KIND_BOOL;
    out.bool_val = false;
    return out;
  }
  IpTag tag = ip_tag(&s->intern, v);
  IpKey k = ip_key(&s->intern, v);
  switch (tag) {
  case IP_TAG_INT_VALUE:
    out.kind = TV_KIND_INT;
    out.type = k.int_value.type;
    out.int_val = k.int_value.value;
    return out;
  case IP_TAG_FLOAT_VALUE:
    out.kind = TV_KIND_FLOAT;
    out.type = k.float_value.type;
    out.float_val = k.float_value.value;
    return out;
  case IP_TAG_NAMESPACE_VALUE:
    out.kind = TV_KIND_NAMESPACE;
    out.nsid = k.namespace_value.nsid.idx;
    return out;
  case IP_TAG_ENUM_VARIANT_VALUE:
    out.kind = TV_KIND_ENUM_VARIANT;
    out.enum_def = k.enum_variant_value.enum_def;
    out.variant_idx = k.enum_variant_value.variant_idx;
    return out;
  default:
    return out;
  }
}

// -------------------------------------------------------------------
// tv_value_semantic_eq
// -------------------------------------------------------------------

bool tv_value_semantic_eq(struct db *s, IpIndex a, IpIndex b) {
  // Fast path: identical interned values.
  if (a.v == b.v && a.v != IP_NONE.v && !ip_is_error(a))
    return true;
  TvDecoded da = tv_decode(s, a);
  TvDecoded db_ = tv_decode(s, b);
  if (da.kind == TV_KIND_NONE || db_.kind == TV_KIND_NONE)
    return false;
  if (da.kind != db_.kind)
    return false;
  switch (da.kind) {
  case TV_KIND_INT: {
    // Same int64_t bit pattern → equal. comptime_int vs concrete int with
    // same numeric value: bit patterns match (parse_int_literal stores
    // the bit pattern; type discriminates how it's interpreted).
    return da.int_val == db_.int_val;
  }
  case TV_KIND_FLOAT: {
    uint64_t ab, bb;
    memcpy(&ab, &da.float_val, sizeof ab);
    memcpy(&bb, &db_.float_val, sizeof bb);
    return ab == bb;
  }
  case TV_KIND_BOOL:
    return da.bool_val == db_.bool_val;
  case TV_KIND_NAMESPACE:
    return da.nsid == db_.nsid;
  case TV_KIND_ENUM_VARIANT:
    return da.enum_def.idx == db_.enum_def.idx &&
           da.variant_idx == db_.variant_idx;
  default:
    return false;
  }
}

// -------------------------------------------------------------------
// tv_fits_in — mirror of db_const_value_fits_in, decoded from IpIndex.
// -------------------------------------------------------------------

bool tv_fits_in(struct db *s, IpIndex value, IpIndex target_type,
                const char **out_lo, const char **out_hi) {
  const char *lo_tmp = NULL, *hi_tmp = NULL;
  if (!out_lo) out_lo = &lo_tmp;
  if (!out_hi) out_hi = &hi_tmp;
  *out_lo = NULL;
  *out_hi = NULL;
  TvDecoded d = tv_decode(s, value);
  if (d.kind == TV_KIND_NONE) return false;

  if (d.kind == TV_KIND_INT) {
    if (target_type.v == IP_COMPTIME_INT_TYPE.v) return true;
    int bits = int_bits(target_type);
    if (bits == 0) return false;
    // Source signedness: comptime_int has no signedness; concrete ints carry
    // it via .type. When source is comptime_int, the bit pattern is the
    // truth — let target signedness decide the read.
    bool src_unsigned = (d.type.v != IP_COMPTIME_INT_TYPE.v) &&
                        tv_type_is_unsigned(d.type);
    if (src_unsigned) {
      if (!is_unsigned_int(target_type)) {
        uint64_t u = (uint64_t)d.int_val;
        if (is_signed_int(target_type)) {
          uint64_t smax = (bits == 64)
              ? (uint64_t)INT64_MAX
              : ((uint64_t)1 << (bits - 1)) - 1;
          if (u > smax)
            return int_fits_signed(d.int_val, bits, out_lo, out_hi);
          return int_fits_signed((int64_t)u, bits, out_lo, out_hi);
        }
        return false;
      }
      return int_fits_unsigned_u64((uint64_t)d.int_val, bits, out_lo, out_hi);
    }
    if (is_signed_int(target_type))
      return int_fits_signed(d.int_val, bits, out_lo, out_hi);
    if (is_unsigned_int(target_type))
      return int_fits_unsigned(d.int_val, bits, out_lo, out_hi);
    return false;
  }
  if (d.kind == TV_KIND_FLOAT) {
    if (target_type.v == IP_COMPTIME_FLOAT_TYPE.v ||
        target_type.v == IP_F64_TYPE.v) return true;
    if (target_type.v == IP_F32_TYPE.v) {
      double a = d.float_val < 0 ? -d.float_val : d.float_val;
      return a == 0.0 || a <= 3.4028234663852886e38;
    }
    return false;
  }
  if (d.kind == TV_KIND_BOOL)
    return target_type.v == IP_BOOL_TYPE.v;
  return false;
}

// -------------------------------------------------------------------
// tv_value_to_str
// -------------------------------------------------------------------

const char *tv_value_to_str(struct db *s, IpIndex value, char *buf,
                            size_t buflen) {
  if (!buf || buflen < 2) return "?";
  TvDecoded d = tv_decode(s, value);
  switch (d.kind) {
  case TV_KIND_INT:
    // Unsigned source ≥ 2^63 reads as negative int64_t — render via u64.
    if (d.type.v != IP_COMPTIME_INT_TYPE.v && tv_type_is_unsigned(d.type))
      snprintf(buf, buflen, "%llu", (unsigned long long)(uint64_t)d.int_val);
    else
      snprintf(buf, buflen, "%lld", (long long)d.int_val);
    break;
  case TV_KIND_FLOAT:
    snprintf(buf, buflen, "%g", d.float_val);
    break;
  case TV_KIND_BOOL:
    snprintf(buf, buflen, "%s", d.bool_val ? "true" : "false");
    break;
  default:
    snprintf(buf, buflen, "?");
    break;
  }
  return buf;
}

// -------------------------------------------------------------------
// tv_value_in_range
// -------------------------------------------------------------------

bool tv_value_in_range(const SemaCtx *ctx, SyntaxNode *range_node,
                       IpIndex scrut_value) {
  if (!range_node) return false;
  if (syntax_node_kind(range_node) != SK_BIN_EXPR) return false;
  BinExpr be;
  if (!BinExpr_cast(range_node, &be)) return false;
  SyntaxKind opk = BinExpr_op_kind(&be);
  if (opk != SK_DOT_DOT_LT && opk != SK_DOT_DOT_EQ) return false;
  SyntaxNode *lo_n = BinExpr_lhs(&be);
  SyntaxNode *hi_n = BinExpr_rhs(&be);
  TypedValue lo_tv = lo_n ? eval_expr(ctx, lo_n) : TYPED_VALUE_NONE;
  TypedValue hi_tv = hi_n ? eval_expr(ctx, hi_n) : TYPED_VALUE_NONE;
  if (lo_n) syntax_node_release(lo_n);
  if (hi_n) syntax_node_release(hi_n);
  TvDecoded sd = tv_decode(ctx->s, scrut_value);
  TvDecoded ld = tv_decode(ctx->s, lo_tv.value);
  TvDecoded hd = tv_decode(ctx->s, hi_tv.value);
  if (sd.kind != TV_KIND_INT || ld.kind != TV_KIND_INT || hd.kind != TV_KIND_INT)
    return false;
  int64_t v = sd.int_val, lo = ld.int_val, hi = hd.int_val;
  if (opk == SK_DOT_DOT_EQ)
    return v >= lo && v <= hi;
  return v >= lo && v < hi;  // SK_DOT_DOT_LT
}

// -------------------------------------------------------------------
// tv_int_compare
// -------------------------------------------------------------------

IpIndex tv_int_compare(struct db *s, IpIndex l, IpIndex r, SyntaxKind opk) {
  TvDecoded dl = tv_decode(s, l);
  TvDecoded dr = tv_decode(s, r);
  if (dl.kind != TV_KIND_INT || dr.kind != TV_KIND_INT)
    return IP_NONE;
  // Choose unsigned compare when EITHER operand is u-typed (matches J2's
  // either_unsigned rule). comptime_int operands inherit the typed
  // operand's signedness; if both are comptime_int, default to signed.
  bool l_unsigned = (dl.type.v != IP_COMPTIME_INT_TYPE.v) &&
                    tv_type_is_unsigned(dl.type);
  bool r_unsigned = (dr.type.v != IP_COMPTIME_INT_TYPE.v) &&
                    tv_type_is_unsigned(dr.type);
  bool use_unsigned = l_unsigned || r_unsigned;
  bool result;
  if (use_unsigned) {
    uint64_t lu = (uint64_t)dl.int_val;
    uint64_t ru = (uint64_t)dr.int_val;
    switch (opk) {
    case SK_LT: result = lu <  ru; break;
    case SK_LE: result = lu <= ru; break;
    case SK_GT: result = lu >  ru; break;
    case SK_GE: result = lu >= ru; break;
    default: return IP_NONE;
    }
  } else {
    int64_t lv = dl.int_val, rv = dr.int_val;
    switch (opk) {
    case SK_LT: result = lv <  rv; break;
    case SK_LE: result = lv <= rv; break;
    case SK_GT: result = lv >  rv; break;
    case SK_GE: result = lv >= rv; break;
    default: return IP_NONE;
    }
  }
  return result ? IP_BOOL_TRUE : IP_BOOL_FALSE;
}
