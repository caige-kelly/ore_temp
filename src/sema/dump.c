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
  ScopeId export_scope = db_get_module_export_scope(s, mid);
  ScopeId internal_scope = db_get_module_internal_scope(s, mid);

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
    printf("Name resolution: %u/%u round-trip ok\n\n", int_end - int_start,
           int_end - int_start);

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
    // db_query_infer_body returned a non-NONE sig above, so `def` is a
    // function — read its body-scope binds from the shared pool.
    FnBody fb =
        *(FnBody *)vec_get(&s->fns.body, db_def_row(s, def, KIND_FUNCTION));
    const ScopedBind *binds = (const ScopedBind *)s->body_scope_binds.data;
    for (uint32_t e = 0; e < fb.bind_len; e++) {
      const ScopedBind *bd = &binds[fb.bind_off + e];
      char tb[256];
      db_format_type(s, bd->type, tb, sizeof tb);
      printf("    local %-16s : %s  (scope %u)\n",
             pool_get(&s->strings, bd->name), tb, bd->scope_id);
    }
    if (fb.bind_len == 0)
      printf("    (no params)\n");
    fn_count++;
  }
  if (fn_count == 0)
    printf("  (no fn defs)\n");
  printf("\n");
}
