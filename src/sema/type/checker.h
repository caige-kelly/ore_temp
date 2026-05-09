#ifndef ORE_SEMA_TYPE_CHECKER_H
#define ORE_SEMA_TYPE_CHECKER_H

#include "../ids/ids.h"

struct Sema;
struct Type;

// Compute the type of a top-level decl. Slot lives on
// `SemaDeclInfo.type_query` so cycle detection + invalidation
// flow through the standard query machinery.
//
// Behavior, by Bind shape:
//   - `x : T = v`  / `x : T :: v` — type = resolve(T) in NS_TYPE.
//                                  Const-eval `v`; range-check it
//                                  fits in T; emit diagnostic on
//                                  miss. Return T.
//   - `x :: v` / `x := v`         — type = inferred from v.
//                                  Numeric literal → comptime_int /
//                                  comptime_float. Other shapes
//                                  return `error_type` for now;
//                                  Stage E.2+ widens this.
//
// Returns Sema's `error_type` on any failure; never NULL.
struct Type *query_type_of_decl(struct Sema *s, DefId def);

#endif
