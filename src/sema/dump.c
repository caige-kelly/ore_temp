#include "../db/db.h"
#include "../db/intern_pool/intern_pool.h"
#include "../db/query/def_identity.h"
#include "../db/query/infer_body.h"
#include "../db/query/resolve_ref.h"
#include "../db/query/type_of_def.h"
#include "../db/storage/stringpool.h"
#include "sema.h"

#include <stdint.h>
#include <stdio.h>

// === Main dump =============================================================
//
// Type rendering delegates to db_format_type (src/db/db.c). Sema-level
// dump and diag rendering share that one impl so nominal types render
// consistently as "struct Foo" / "enum Bar" rather than "struct#42".

void sema_dump_module(struct db *s, ModuleId mid) {
  ScopeId export_scope = SCOPE_ID_NONE;
  ScopeId internal_scope = SCOPE_ID_NONE;
  if (mid.idx < s->modules.exports.count)
    export_scope = *(ScopeId *)vec_get(&s->modules.exports, mid.idx);
  if (mid.idx < s->modules.internal_scopes.count)
    internal_scope =
        *(ScopeId *)vec_get(&s->modules.internal_scopes, mid.idx);

  if (export_scope.idx == SCOPE_ID_NONE.idx)
    return;

  // === Export scope: pubs with types ===
  printf("\nTop-Level Defs (export scope):\n");
  uint32_t off_start =
      *(uint32_t *)vec_get(&s->scopes.decl_offsets, export_scope.idx);
  uint32_t off_end =
      *(uint32_t *)vec_get(&s->scopes.decl_offsets, export_scope.idx + 1);
  for (uint32_t i = off_start; i < off_end; i++) {
    DeclEntry *de = (DeclEntry *)vec_get(&s->scopes.decl_pool, i);
    DefId def = db_query_def_identity(s, mid, de->ast_id);
    IpIndex t = db_query_type_of_def(s, def);
    char tbuf[256];
    db_format_type(s, t, tbuf, sizeof tbuf);
    printf("  def=%u  name=%-20s ast_id=%08x  type=%s\n", def.idx,
           pool_get(&s->strings, de->name), de->ast_id.idx, tbuf);
  }
  uint32_t int_start = 0, int_end = 0;
  if (internal_scope.idx != SCOPE_ID_NONE.idx) {
    int_start =
        *(uint32_t *)vec_get(&s->scopes.decl_offsets, internal_scope.idx);
    int_end =
        *(uint32_t *)vec_get(&s->scopes.decl_offsets, internal_scope.idx + 1);
  }
  printf("  (internal scope: %u defs, export scope: %u defs)\n\n",
         int_end - int_start, off_end - off_start);

  if (internal_scope.idx == SCOPE_ID_NONE.idx)
    return;

  // === Name-resolution sanity check ===
  uint32_t mismatches = 0;
  for (uint32_t i = int_start; i < int_end; i++) {
    DeclEntry *de = (DeclEntry *)vec_get(&s->scopes.decl_pool, i);
    DefId expected = db_query_def_identity(s, mid, de->ast_id);
    DefId resolved = db_query_resolve_ref(s, internal_scope, de->name);
    if (resolved.idx != expected.idx) {
      printf("  [resolve mismatch] name=%s resolved=%u expected=%u\n",
             pool_get(&s->strings, de->name), resolved.idx, expected.idx);
      mismatches++;
    }
  }
  if (mismatches == 0)
    printf("Name resolution: %u/%u round-trip ok\n\n",
           int_end - int_start, int_end - int_start);

  // === Internal-scope top-level types ===
  printf("Top-Level Types (internal scope):\n");
  for (uint32_t i = int_start; i < int_end; i++) {
    DeclEntry *de = (DeclEntry *)vec_get(&s->scopes.decl_pool, i);
    DefId def = db_query_def_identity(s, mid, de->ast_id);
    IpIndex t = db_query_type_of_def(s, def);
    char tbuf[256];
    db_format_type(s, t, tbuf, sizeof tbuf);
    printf("  def=%u  name=%-20s type=%s\n", def.idx,
           pool_get(&s->strings, de->name), tbuf);
  }
  printf("\n");

  // === Per-fn local scopes ===
  printf("Body Inference (per-fn local scope):\n");
  size_t fn_count = 0;
  for (uint32_t i = int_start; i < int_end; i++) {
    DeclEntry *de = (DeclEntry *)vec_get(&s->scopes.decl_pool, i);
    DefId def = db_query_def_identity(s, mid, de->ast_id);
    IpIndex sig = db_query_infer_body(s, def);
    if (sig.v == IP_NONE.v)
      continue;
    char tbuf[256];
    db_format_type(s, sig, tbuf, sizeof tbuf);
    printf("  fn %s : %s\n", pool_get(&s->strings, de->name), tbuf);
    BodyScopes *bs = NULL;
    if (def.idx < s->defs.body_scopes.count)
      bs = *(BodyScopes **)vec_get(&s->defs.body_scopes, def.idx);
    if (bs) {
      ScopedBind *binds = (ScopedBind *)bs->binds.data;
      for (size_t e = 0; e < bs->binds.count; e++) {
        char tb[256];
        db_format_type(s, binds[e].type, tb, sizeof tb);
        printf("    local %-16s : %s  (scope %u)\n",
               pool_get(&s->strings, binds[e].name), tb, binds[e].scope_id);
      }
      if (bs->binds.count == 0)
        printf("    (no params)\n");
    } else {
      printf("    (no scope)\n");
    }
    fn_count++;
  }
  if (fn_count == 0)
    printf("  (no fn defs)\n");
  printf("\n");
}
