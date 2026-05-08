#ifndef ORE_SEMA_EFFECT_OPS_H
#define ORE_SEMA_EFFECT_OPS_H

#include "../../common/vec.h"
#include "../ids/ids.h"

// Effect-op visibility — which op DefIds are bare-name reachable
// inside a function body.
//
// When a function carries an effect annotation `<E>`, every op in
// E becomes a bare-name reference inside its body. So
// `fn read() <Reader>` lets `read_byte()` resolve without
// qualification because Reader's `read_byte` op is brought into
// scope.
//
// query_effect_ops_visible(fn_def) returns the Vec<DefId> of ops
// thus injected. The set is computed from the function's effect
// annotation by:
//   1. Walking the annotation AST.
//   2. For each named effect, query_resolve_ref to its DefId.
//   3. Reading the effect's child_scope (the SCOPE_EFFECT holding
//      its op decls).
//   4. Concatenating each effect's ops into the result Vec.
//
// Cached in Sema.effect_ops_cache. The result Vec is arena-owned;
// callers may iterate but not mutate.

struct Sema;

// Returns a Vec<DefId> of ops visible by bare name inside the
// body of `fn_def`. Returns NULL if `fn_def` is invalid or has
// no effect annotation. The returned Vec may be empty (zero
// effects in the annotation, or all annotation effects failed
// to resolve); callers should treat NULL and empty Vec
// identically for "no effect ops visible."
Vec *query_effect_ops_visible(struct Sema *s, DefId fn_def);

#endif // ORE_SEMA_EFFECT_OPS_H
