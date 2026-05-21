#include "../../sema/sema.h"
#include "../db.h"
#include "fn_signature.h"
#include "query.h"
#include "query_engine.h"

// Thin salsa wrapper. Slot in db.defs.slots_signature[def.idx]; result
// stored in db.defs.fn_sigs[def.idx]. Separate from db.defs.types
// (owned by type_of_def) because fn_signature returns IP_NONE for
// non-fn defs — sharing storage would clobber type_of_def's valid
// result every time infer_body called fn_signature on a non-fn.
IpIndex db_query_fn_signature(struct db *s, DefId def) {
  if (def.idx == DEF_ID_NONE.idx)
    return IP_NONE;

  DB_QUERY_GUARD(s, QUERY_FN_SIGNATURE, &def,
                 *(IpIndex *)vec_get(&s->defs.fn_sigs, def.idx), IP_NONE,
                 IP_NONE);

  IpIndex result = sema_fn_signature(s, def);

  *(IpIndex *)vec_get(&s->defs.fn_sigs, def.idx) = result;
  Fingerprint fp = db_fp_u64((uint64_t)def.idx);
  fp = db_fp_combine(fp, db_fp_u64((uint64_t)result.v));
  db_query_succeed(s, QUERY_FN_SIGNATURE, &def, fp);
  return result;
}
