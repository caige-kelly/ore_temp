#include "body_scopes.h"
#include "../../sema/sema.h"
#include "../db.h"
#include "query.h"
#include "query_engine.h"

// Thin salsa wrapper. The body-scope tree is built into the shared db
// pools; db.fns.body[row] holds this fn's (off,len) ranges. The wrapper
// returns only success — callers use db_query_body_scopes for the salsa
// dep + early cutoff, never for a value.
bool db_query_body_scopes(struct db *s, DefId fn_def) {
  if (fn_def.idx == DEF_ID_NONE.idx)
    return false;
  // Body scopes exist only for function defs. Callers may query blindly.
  if (db_def_kind(s, fn_def) != KIND_FUNCTION)
    return false;

  DB_QUERY_GUARD(s, QUERY_BODY_SCOPES, (uint64_t)fn_def.idx, true, false,
                 false);

  FnBody fb = sema_body_scopes(s, fn_def);

  // Fingerprint folds the body's scope-tree shape + binds. Stable across
  // cached calls; an AST edit that doesn't change bindings (comment,
  // whitespace) reproduces the same fp → consumers early-cutoff.
  Fingerprint fp = db_fp_u64((uint64_t)fn_def.idx);
  fp = db_fp_combine(fp, db_fp_u64((uint64_t)fb.scope_len));
  fp = db_fp_combine(fp, db_fp_u64((uint64_t)fb.bind_len));
  const ScopeRow *rows = (const ScopeRow *)s->body_scope_rows.data;
  for (uint32_t i = 0; i < fb.scope_len; i++) {
    const ScopeRow *r = &rows[fb.scope_off + i];
    fp = db_fp_combine(fp, db_fp_u64((uint64_t)r->parent));
    fp = db_fp_combine(fp, db_fp_u64((uint64_t)r->block_node.idx));
  }
  const ScopedBind *binds = (const ScopedBind *)s->body_scope_binds.data;
  for (uint32_t i = 0; i < fb.bind_len; i++) {
    const ScopedBind *bd = &binds[fb.bind_off + i];
    fp = db_fp_combine(fp, db_fp_u64((uint64_t)bd->scope_id));
    fp = db_fp_combine(fp, db_fp_u64((uint64_t)bd->name.idx));
    fp = db_fp_combine(fp, db_fp_u64((uint64_t)bd->type.v));
  }

  db_query_succeed(s, QUERY_BODY_SCOPES, (uint64_t)fn_def.idx, fp);
  return true;
}
