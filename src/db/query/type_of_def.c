#include "type_of_def.h"
#include "../../sema/sema.h"
#include "../db.h"
#include "query.h"
#include "query_engine.h"

// Thin salsa wrapper. The DB layer owns the slot lifecycle (DB_QUERY_GUARD
// → compute → db_query_succeed); the semantic computation lives in
// sema/type_of_def.c. The result is stored in the def's per-kind `type`
// column — db_def_type_cell routes by DefKind.
IpIndex db_query_type_of_def(struct db *s, DefId def) {
  if (def.idx == DEF_ID_NONE.idx)
    return IP_NONE;

  // Synthetic primitives bypass the slot machinery entirely — they have
  // KIND_NONE so db_def_type_cell's per-kind assertion would trip, and
  // their type is immortal so there's no salsa dep to record. Short-
  // circuit before DB_QUERY_GUARD allocates a slot or registers any
  // dependency.
  IpIndex prim = db_primitive_type_for(s, def);
  if (prim.v != IP_NONE.v)
    return prim;

  DB_QUERY_GUARD(s, QUERY_TYPE_OF_DECL, (uint64_t)def.idx,
                 *db_def_type_cell(s, def),
                 IP_NONE, IP_NONE);

  IpIndex result = sema_type_of_def(s, def);

  // Re-fetch the cell — sema_type_of_def can grow a per-kind table
  // (classifying a referenced def), invalidating any earlier pointer.
  *db_def_type_cell(s, def) = result;
  Fingerprint fp = db_fp_u64((uint64_t)def.idx);
  fp = db_fp_combine(fp, db_fp_u64((uint64_t)result.v));
  db_query_succeed(s, QUERY_TYPE_OF_DECL, (uint64_t)def.idx, fp);
  return result;
}
