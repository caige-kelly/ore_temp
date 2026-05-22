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
  // ScopeRow (2×u32) and ScopedBind (3×u32) are padding-free PODs and
  // each (off,len) slice is contiguous in its pool — hash the whole
  // slice in one db_fp_bytes block pass rather than element-wise
  // db_fp_combine, which would defeat the word-at-a-time block hash.
  if (fb.scope_len) {
    const ScopeRow *rows = (const ScopeRow *)s->body_scope_rows.data;
    fp = db_fp_combine(fp, db_fp_bytes(&rows[fb.scope_off],
                                       (size_t)fb.scope_len * sizeof(ScopeRow)));
  }
  if (fb.bind_len) {
    const ScopedBind *binds = (const ScopedBind *)s->body_scope_binds.data;
    fp = db_fp_combine(
        fp, db_fp_bytes(&binds[fb.bind_off],
                        (size_t)fb.bind_len * sizeof(ScopedBind)));
  }

  db_query_succeed(s, QUERY_BODY_SCOPES, (uint64_t)fn_def.idx, fp);
  return true;
}
