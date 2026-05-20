#include "../../sema/sema.h"
#include "../db.h"
#include "infer_body.h"
#include "query.h"
#include "query_engine.h"

// Thin salsa wrapper. Slot in db.defs.slots_infer[def.idx]; shares
// db.defs.types[def.idx] as the result-storage column with fn_signature
// (both return the same IpIndex for a fn def). Sema-side body inference
// populates db.defs.local_scopes[def.idx] as a side effect.
IpIndex db_query_infer_body(struct db *s, DefId def) {
  if (def.idx == DEF_ID_NONE.idx)
    return IP_NONE;

  DB_QUERY_GUARD(s, QUERY_INFER_BODY, &def,
                 *(IpIndex *)vec_get(&s->defs.types, def.idx), IP_NONE,
                 IP_NONE);

  IpIndex result = sema_infer_body(s, def);

  Fingerprint fp = db_fp_u64((uint64_t)def.idx);
  fp = db_fp_combine(fp, db_fp_u64((uint64_t)result.v));
  db_query_succeed(s, QUERY_INFER_BODY, &def, fp);
  return result;
}
