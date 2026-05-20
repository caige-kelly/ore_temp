#ifndef ORE_DB_QUERY_INFER_BODY_H
#define ORE_DB_QUERY_INFER_BODY_H

#include "../db.h"
#include "../intern_pool/intern_pool.h"

// Per-fn body inference. Builds the fn's local scope (params + later
// let-binds) into db.defs.local_scopes[def.idx] and records the salsa
// deps that drive its invalidation. Returns the same IpIndex as
// db_query_fn_signature (the fn's interned type) on success, IP_NONE
// on failure or for non-fn defs.
//
// Chunk 5b scope:
//   - Builds local scope from the lambda's params only. Let-binds
//     inside the body, expression typing of the body, and diagnostics
//     all land in subsequent sub-chunks.
//
// Why a separate query (vs. folding into type_of_def):
//   - Local-scope construction is per-fn; the result lives for the
//     fn's lifetime. type_of_def's fingerprint is fn-shape-only.
//   - Expression typing inside a body will surface diagnostics; those
//     belong on this slot, so revalidation can replay them per-fn
//     rather than invalidating every top-level decl on any change.
//   - Caches the local scope so AST_EXPR_PATH lookups inside the body
//     (in subsequent chunks) hit a precomputed map instead of
//     re-walking the lambda's params each time.
IpIndex db_query_infer_body(struct db *s, DefId def);

// (Local-scope lookup helper now lives in sema/ as sema_local_scope_lookup —
// the lookup is a sema concern that uses db.defs.local_scopes as its
// storage.)

#endif // ORE_DB_QUERY_INFER_BODY_H
