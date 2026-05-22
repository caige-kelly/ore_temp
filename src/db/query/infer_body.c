#include "infer_body.h"
#include "../../sema/sema.h"
#include "../db.h"
#include "query.h"
#include "query_engine.h"

// Thin salsa wrapper. Slot in db.fns.slot_infer. The return value IS the
// fn signature, so on_cached reads db.fns.signature (db_fn_signature_cell)
// — infer_body has no result column of its own. Its real work is the
// body type-check plus the dep it records on QUERY_BODY_SCOPES.
IpIndex db_query_infer_body(struct db *s, DefId def) {
  if (def.idx == DEF_ID_NONE.idx)
    return IP_NONE;
  // Body inference applies only to function defs; non-fns have no
  // db.fns row (and no INFER_BODY slot). Callers may query blindly.
  if (db_def_kind(s, def) != KIND_FUNCTION)
    return IP_NONE;

  DB_QUERY_GUARD(s, QUERY_INFER_BODY, (uint64_t)def.idx,
                 *db_fn_signature_cell(s, def),
                 IP_NONE, IP_NONE);

  IpIndex result = sema_infer_body(s, def);

  Fingerprint fp = db_fp_u64((uint64_t)def.idx);
  fp = db_fp_combine(fp, db_fp_u64((uint64_t)result.v));
  db_query_succeed(s, QUERY_INFER_BODY, (uint64_t)def.idx, fp);
  return result;
}
