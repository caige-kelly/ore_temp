#include "fn_signature.h"
#include "../../sema/sema.h"
#include "../db.h"
#include "query.h"
#include "query_engine.h"

// Thin salsa wrapper. Slot + result live in db.fns (slot_signature /
// signature), reached via db_fn_signature_cell — fn_signature is only
// ever queried on a function def.
IpIndex db_query_fn_signature(struct db *s, DefId def) {
  if (def.idx == DEF_ID_NONE.idx)
    return IP_NONE;
  // A signature is defined only on function defs; non-fns have no
  // db.fns row (and no FN_SIGNATURE slot). Callers may query blindly.
  if (db_def_kind(s, def) != KIND_FUNCTION)
    return IP_NONE;

  DB_QUERY_GUARD(s, QUERY_FN_SIGNATURE, &def, *db_fn_signature_cell(s, def),
                 IP_NONE, IP_NONE);

  IpIndex result = sema_fn_signature(s, def);

  // Re-fetch — sema_fn_signature can grow a per-kind table.
  *db_fn_signature_cell(s, def) = result;
  Fingerprint fp = db_fp_u64((uint64_t)def.idx);
  fp = db_fp_combine(fp, db_fp_u64((uint64_t)result.v));
  db_query_succeed(s, QUERY_FN_SIGNATURE, &def, fp);
  return result;
}
