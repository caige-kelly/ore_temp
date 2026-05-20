#include "fits.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "type.h"

// We treat the host's int64_t as the carrier for comptime_int. The
// signed-vs-unsigned int range checks compare against this carrier
// value directly: a comptime_int is in range for u8 iff it's in
// [0, 255], for i8 iff [−128, 127], etc.
//
// `usize` and `isize` are sized at the host's pointer width here.
// When the type system grows a target-info notion this should consult
// the target rather than the host.

static bool int_fits_signed(int64_t v, int bits, const char **lo,
                            const char **hi) {
  switch (bits) {
  case 8:
    *lo = "-128";
    *hi = "127";
    return v >= INT8_MIN && v <= INT8_MAX;
  case 16:
    *lo = "-32768";
    *hi = "32767";
    return v >= INT16_MIN && v <= INT16_MAX;
  case 32:
    *lo = "-2147483648";
    *hi = "2147483647";
    return v >= INT32_MIN && v <= INT32_MAX;
  case 64:
    *lo = "-9223372036854775808";
    *hi = "9223372036854775807";
    return true; // i64 holds the carrier
  }
  return false;
}

static bool int_fits_unsigned(int64_t v, int bits, const char **lo,
                              const char **hi) {
  if (v < 0) {
    *lo = "0";
    switch (bits) {
    case 8:
      *hi = "255";
      break;
    case 16:
      *hi = "65535";
      break;
    case 32:
      *hi = "4294967295";
      break;
    case 64:
      *hi = "18446744073709551615";
      break;
    }
    return false;
  }
  uint64_t u = (uint64_t)v;
  switch (bits) {
  case 8:
    *lo = "0";
    *hi = "255";
    return u <= UINT8_MAX;
  case 16:
    *lo = "0";
    *hi = "65535";
    return u <= UINT16_MAX;
  case 32:
    *lo = "0";
    *hi = "4294967295";
    return u <= UINT32_MAX;
  case 64:
    *lo = "0";
    *hi = "18446744073709551615";
    return true;
  }
  return false;
}

static bool float_fits(double v, int bits, const char **lo, const char **hi) {
  if (!isfinite(v)) {
    *lo = NULL;
    *hi = NULL;
    return false;
  }
  double abs = v < 0 ? -v : v;
  switch (bits) {
  case 32:
    *lo = "-3.4e+38";
    *hi = "3.4e+38";
    return abs <= (double)FLT_MAX;
  case 64:
    *lo = "-1.7e+308";
    *hi = "1.7e+308";
    return abs <= DBL_MAX;
  }
  return false;
}

bool fits_in(struct ConstValue v, const struct Type *t, const char **out_min,
             const char **out_max) {
  const char *lo = NULL, *hi = NULL;
  bool ok = false;

  if (!t || t->kind == TY_ERROR) {
    if (out_min)
      *out_min = NULL;
    if (out_max)
      *out_max = NULL;
    return false;
  }

  if (v.kind == CONST_INT) {
    switch (t->kind) {
    case TY_I8:
      ok = int_fits_signed(v.int_val, 8, &lo, &hi);
      break;
    case TY_I16:
      ok = int_fits_signed(v.int_val, 16, &lo, &hi);
      break;
    case TY_I32:
      ok = int_fits_signed(v.int_val, 32, &lo, &hi);
      break;
    case TY_I64:
      ok = int_fits_signed(v.int_val, 64, &lo, &hi);
      break;
    case TY_ISIZE:
      ok = int_fits_signed(v.int_val, 64, &lo, &hi);
      break;
    case TY_U8:
      ok = int_fits_unsigned(v.int_val, 8, &lo, &hi);
      break;
    case TY_U16:
      ok = int_fits_unsigned(v.int_val, 16, &lo, &hi);
      break;
    case TY_U32:
      ok = int_fits_unsigned(v.int_val, 32, &lo, &hi);
      break;
    case TY_U64:
      ok = int_fits_unsigned(v.int_val, 64, &lo, &hi);
      break;
    case TY_USIZE:
      ok = int_fits_unsigned(v.int_val, 64, &lo, &hi);
      break;
    case TY_COMPTIME_INT:
      ok = true;
      break;
    default:
      ok = false;
      break;
    }
  } else if (v.kind == CONST_FLOAT) {
    switch (t->kind) {
    case TY_F32:
      ok = float_fits(v.float_val, 32, &lo, &hi);
      break;
    case TY_F64:
      ok = float_fits(v.float_val, 64, &lo, &hi);
      break;
    case TY_COMPTIME_FLOAT:
      ok = true;
      break;
    default:
      ok = false;
      break;
    }
  }

  if (out_min)
    *out_min = lo;
  if (out_max)
    *out_max = hi;
  return ok;
}

const char *const_value_to_str(struct ConstValue v, char *buf, size_t buflen) {
  if (!buf || buflen == 0)
    return "";
  switch (v.kind) {
  case CONST_INT:
    snprintf(buf, buflen, "%lld", (long long)v.int_val);
    break;
  case CONST_FLOAT:
    snprintf(buf, buflen, "%g", v.float_val);
    break;
  default:
    snprintf(buf, buflen, "<not constant>");
    break;
  }
  return buf;
}
