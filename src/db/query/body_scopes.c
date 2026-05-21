#include "../../sema/sema.h"
#include "../db.h"
#include "body_scopes.h"
#include "query.h"
#include "query_engine.h"

// Thin salsa wrapper. Slot in db.defs.slots_body_scopes[def.idx]; the
// BodyScopes* result lives in db.defs.body_scopes[def.idx]. The
// pointer is stable across cached calls — the builder reuses storage
// rather than reallocating (vec_clear on the inner Vecs), so deps
// recorded against the pointer remain valid.
BodyScopes *db_query_body_scopes(struct db *s, DefId fn_def) {
  if (fn_def.idx == DEF_ID_NONE.idx)
    return NULL;

  DB_QUERY_GUARD(s, QUERY_BODY_SCOPES, &fn_def,
                 *(BodyScopes **)vec_get(&s->defs.body_scopes, fn_def.idx),
                 NULL, NULL);

  BodyScopes *result = sema_body_scopes(s, fn_def);

  // Fingerprint folds the body's scope-tree shape + binds. Stable
  // across cached calls; an AST edit that doesn't change bindings
  // (e.g. comment, whitespace) reproduces the same fp → consumers
  // early-cutoff.
  Fingerprint fp = db_fp_u64((uint64_t)fn_def.idx);
  if (result) {
    fp = db_fp_combine(fp, db_fp_u64((uint64_t)result->scopes.count));
    fp = db_fp_combine(fp, db_fp_u64((uint64_t)result->binds.count));
    for (size_t i = 0; i < result->scopes.count; i++) {
      ScopeRow *r = (ScopeRow *)vec_get(&result->scopes, i);
      fp = db_fp_combine(fp, db_fp_u64((uint64_t)r->parent));
      fp = db_fp_combine(fp, db_fp_u64((uint64_t)r->block_node.idx));
    }
    for (size_t i = 0; i < result->binds.count; i++) {
      ScopedBind *b = (ScopedBind *)vec_get(&result->binds, i);
      fp = db_fp_combine(fp, db_fp_u64((uint64_t)b->scope_id));
      fp = db_fp_combine(fp, db_fp_u64((uint64_t)b->name.idx));
      fp = db_fp_combine(fp, db_fp_u64((uint64_t)b->type.v));
    }
  }
  db_query_succeed(s, QUERY_BODY_SCOPES, &fn_def, fp);
  return result;
}
