#ifndef ORE_DB_QUERY_FN_SIGNATURE_H
#define ORE_DB_QUERY_FN_SIGNATURE_H

#include "../db.h"
#include "../intern_pool/intern_pool.h"

// Fn signature as an interned fn type. Split from db_query_type_of_def
// so call-site checking can depend on just the signature (params +
// return), without forcing the body's value type to be computed when
// the caller only needs to typecheck arguments.
//
// Returns the same IpIndex that db_query_type_of_def returns for a fn
// def; stored in db.fns.signature[row]. Effect rows are NOT yet part
// of the structural identity — fn type identity is (ret, modifiers,
// params) only. Effect-aware signatures land with chunk 8.
IpIndex db_query_fn_signature(struct db *s, DefId def);

#endif // ORE_DB_QUERY_FN_SIGNATURE_H
