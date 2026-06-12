#ifndef ORE_DB_QUERY_TYPED_VALUE_H
#define ORE_DB_QUERY_TYPED_VALUE_H

#include "../intern_pool/intern_pool.h" // IpIndex, IP_NONE

// Every expression in Ore evaluates to a (type, value) pair. The TYPE
// tells you the expression's category (e.g., IP_TYPE_TYPE for a type
// expression, IP_I32_TYPE for an integer-typed expression). The VALUE
// is the comptime-folded result if known, IP_NONE if the expression is
// runtime-only.
//
// Examples:
//   `5`            → { type: IP_COMPTIME_INT, value: <interned int 5> }
//   `i32`          → { type: IP_TYPE_TYPE,    value: IP_I32_TYPE }
//   `read_input()` → { type: IP_I32_TYPE,     value: IP_NONE }
//   `t` in body    → { type: IP_TYPE_TYPE,    value: <hole or concrete> }
//
// Phase 1 introduces this type and the storage to carry it through the
// existing NodeTypeBuilder by packing both halves into the HashMap's
// 64-bit slot (lower 32 = type.v, upper 32 = value.v). IP_NONE.v == 0
// so a legacy "type-only" push produces a byte-identical encoding.
//
// Phase 2+ uses this to unify the three split evaluators
// (type_of_expr / const_eval / resolve_type_expr) into one. See
// plans/phase-b-architecture-review.md for the full architecture.
typedef struct TypedValue {
  IpIndex type;
  IpIndex value;
} TypedValue;

#define TYPED_VALUE_NONE                                                       \
  ((TypedValue){.type = IP_NONE, .value = IP_NONE})

#endif // ORE_DB_QUERY_TYPED_VALUE_H
