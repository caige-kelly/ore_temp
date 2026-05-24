#include "infer_body.h"
#include "../../sema/sema.h"
#include "../db.h"
#include "query.h"
#include "query_engine.h"

// Thin salsa wrapper. Slot in db.fns.slot_infer. The return value IS the
// fn signature, so on_cached reads db.fns.signature (db_fn_signature_cell)
// — infer_body has no result column of its own. Its real work is the
// body type-check plus the dep it records on QUERY_BODY_SCOPES.
//
// Phase 7: the slot fingerprint now folds a TYPED-BODY content hash
// (visit_idx, types[i].v) for every AST node inside the body's source
// range. Stable under sibling-decl edits; reflects any body content
// or referenced-type change. Ready for codegen / MIR consumers that
// will record a QUERY_INFER_BODY dep — when their dep_fp mismatches,
// they re-run; when bar's typed body is unchanged, they early-cut.
IpIndex db_query_infer_body(struct db *s, DefId def) {
  if (def.idx == DEF_ID_NONE.idx)
    return IP_NONE;
  // Body inference applies only to function defs; non-fns have no
  // db.fns row (and no INFER_BODY slot). Callers may query blindly.
  if (db_def_kind(s, def) != KIND_FUNCTION)
    return IP_NONE;

  DB_QUERY_GUARD(s, QUERY_INFER_BODY, (uint64_t)def.idx,
                 *db_fn_signature_cell(s, def), IP_NONE, IP_NONE);

  Fingerprint body_fp = 0;
  IpIndex result = sema_infer_body(s, def, &body_fp);

  Fingerprint fp = db_fp_u64((uint64_t)def.idx);
  fp = db_fp_combine(fp, db_fp_u64((uint64_t)result.v));
  fp = db_fp_combine(fp, body_fp); // typed-body content (Phase 7)
  db_query_succeed(s, QUERY_INFER_BODY, (uint64_t)def.idx, fp);
  return result;
}
