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

  // Inline the begin / cycle / error dispatch rather than using
  // DB_QUERY_GUARD, because the cycle path needs custom behaviour: on
  // re-entry into a struct that's mid-build, we want to return the
  // wip's IpIndex (published into the type cell by build_struct_type
  // before its field loop), not the IP_NONE cycle-default. This is what
  // unblocks self-referential / mutually-referential struct types like
  // `Header :: struct { next : ?^Header; ... }` — without it, the
  // recursive type_of_def call bottoms out at IP_NONE and the whole
  // struct fails to build. See plan-file "wire wip-struct cycle return"
  // for the rationale; ip_wip_struct's whole reason for existing is
  // this trampoline.
  {
    QueryBeginResult __qbr =
        db_query_begin(s, QUERY_TYPE_OF_DECL, (uint64_t)def.idx);
    if (__qbr == QUERY_BEGIN_CACHED)
      return *db_def_type_cell(s, def);
    if (__qbr == QUERY_BEGIN_CYCLE) {
      // Cell holds the wip's IpIndex if build_struct_type has reached
      // the publish point; otherwise IP_NONE for queries that don't
      // use the wip pattern (e.g., non-struct cycles). Guard against
      // KIND_NONE — possible if cycle entry happens before def_identity
      // has classified the def. db_def_type_cell would assert; safer
      // to short to IP_NONE.
      if (db_def_kind(s, def) == KIND_NONE)
        return IP_NONE;
      return *db_def_type_cell(s, def);
    }
    if (__qbr == QUERY_BEGIN_ERROR)
      return IP_NONE;
  }

  IpIndex result = sema_type_of_def(s, def);

  // Re-fetch the cell — sema_type_of_def can grow a per-kind table
  // (classifying a referenced def), invalidating any earlier pointer.
  *db_def_type_cell(s, def) = result;
  Fingerprint fp = db_fp_u64((uint64_t)def.idx);
  fp = db_fp_combine(fp, db_fp_u64((uint64_t)result.v));
  db_query_succeed(s, QUERY_TYPE_OF_DECL, (uint64_t)def.idx, fp);
  return result;
}
