#include "../../sema/sema.h"
#include "../db.h"
#include "query.h"
#include "query_engine.h"
#include "type_of_def.h"

// Thin salsa wrapper. The DB layer owns the slot lifecycle (DB_QUERY_GUARD
// → compute → db_query_succeed); the semantic computation lives in
// sema/type_of_def.c.
//
// db.defs.types[def.idx] is the storage column shared with fn_signature
// (both queries return the same IpIndex for a fn def). The fingerprint
// folds in (def.idx, result.v) — that's enough to drive early-cutoff on
// downstream queries.
IpIndex db_query_type_of_def(struct db *s, DefId def) {
  if (def.idx == DEF_ID_NONE.idx)
    return IP_NONE;

  DB_QUERY_GUARD(s, QUERY_TYPE_OF_DECL, &def,
                 *(IpIndex *)vec_get(&s->defs.types, def.idx), IP_NONE,
                 IP_NONE);

  IpIndex result = sema_type_of_def(s, def);

  *(IpIndex *)vec_get(&s->defs.types, def.idx) = result;
  Fingerprint fp = db_fp_u64((uint64_t)def.idx);
  fp = db_fp_combine(fp, db_fp_u64((uint64_t)result.v));
  db_query_succeed(s, QUERY_TYPE_OF_DECL, &def, fp);
  return result;
}
