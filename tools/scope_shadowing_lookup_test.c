// Phase 0 P0 gap G6 — scope shadowing LOOKUP correctness.
//
// scope_shadowing_test (existing) only proves nested binds live in
// distinct scope_ids. It does NOT actually invoke sema_body_scope_lookup
// to verify that a reference at the inner-block use site returns the
// INNER binding, not the outer. The lookup logic could be completely
// broken and that test would still pass.
//
// This test closes that gap by calling sema_body_scope_lookup at two
// specific use sites and asserting the resolved types match the inner
// vs outer bind types.
//
// Fixture:
//   A :: fn(x: f32) f32 {
//       y := x;       // outer y: f32 (param x's type)
//       {
//           y := 2;   // inner y: comptime_int (literal 2)
//           y;        // INNER USE — must lookup → comptime_int
//       };
//       y             // OUTER USE — must lookup → f32
//   }

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/intern_pool/intern_pool.h"
#include "../src/db/query/body_scopes.h"
#include "../src/db/query/def_identity.h"
#include "../src/db/request/request.h"
#include "../src/sema/sema.h"
#include "../src/support/data_structure/stringpool.h"
#include "../src/syntax/syntax.h"

#include <stdio.h>
#include <string.h>

extern Fingerprint db_query_top_level_index(struct db *s, NamespaceId mod);

int main(void) {
  struct db db;
  db_init(&db);

  const char *path = "shadow_lookup.ore";
  const char *text =
      "A :: fn(x: f32) f32 {\n"  // line 0
      "    y := x;\n"            // line 1: outer y = x (f32)
      "    {\n"                  // line 2
      "        y := 2;\n"        // line 3: inner y = 2 (comptime_int)
      "        y;\n"             // line 4: INNER USE
      "    };\n"                 // line 5
      "    y\n"                  // line 6: OUTER USE
      "}\n";                     // line 7

  SourceId src = db_create_source(&db, path, strlen(path), text, strlen(text));
  NamespaceId M = db_create_namespace(&db);
  FileId fid = db_create_file(&db, src, M);

  int ok = 1;

  db_request_begin(&db, 1);
  (void)db_query_top_level_index(&db, M);
  sema_check_module(&db, M);

  // Find A's DefId.
  ScopeId internal = db_get_namespace_internal_scope(&db, M);
  uint32_t s0 = *(uint32_t *)vec_get(&db.scopes.decl_lo, internal.idx);
  uint32_t s1 = s0 + *(uint32_t *)vec_get(&db.scopes.decl_len, internal.idx);

  DefId defA = DEF_ID_NONE;
  for (uint32_t i = s0; i < s1; i++) {
    DeclEntry *de = (DeclEntry *)vec_get(&db.scopes.decl_pool, i);
    const char *name = pool_get(&db.strings, de->name);
    if (name && strcmp(name, "A") == 0) {
      defA = db_query_def_identity(&db, M, de->node_ptr);
      break;
    }
  }
  if (!def_id_valid(defA)) {
    fprintf(stderr, "FAIL: couldn't find DefId for A\n");
    db_request_end(&db);
    db_free(&db);
    printf("FAIL scope_shadowing_lookup\n");
    return 1;
  }
  (void)db_query_body_scopes(&db, defA);

  // Pre-intern "y" for the lookup name parameter.
  StrId y_name = pool_intern(&db.strings, "y", 1);

  // ---- Inner use site: line 4, col 8 ("        y;") ----
  uint32_t inner_off = db_byte_offset_at(&db, fid, /*line=*/4, /*col=*/8);
  if (inner_off == UINT32_MAX) {
    fprintf(stderr, "FAIL: couldn't compute inner-use byte offset\n");
    ok = 0;
  }
  SyntaxNode *inner_use = inner_off != UINT32_MAX
                              ? db_node_at_offset(&db, fid, inner_off)
                              : NULL;
  if (!inner_use) {
    fprintf(stderr, "FAIL: no SyntaxNode at inner-use position (line 4, col 8)\n");
    ok = 0;
  } else {
    bool found = false;
    IpIndex inner_type =
        sema_body_scope_lookup(&db, defA, inner_use, y_name, &found);
    if (!found) {
      fprintf(stderr, "FAIL: inner-use lookup didn't find 'y'\n");
      ok = 0;
    } else if (inner_type.v != IP_COMPTIME_INT_TYPE.v) {
      fprintf(stderr,
              "FAIL: inner-use 'y' type = %u, want IP_COMPTIME_INT_TYPE=%u "
              "(inner bind is `y := 2` → comptime_int)\n",
              inner_type.v, IP_COMPTIME_INT_TYPE.v);
      ok = 0;
    }
    syntax_node_release(inner_use);
  }

  // ---- Outer use site: line 6, col 4 ("    y") ----
  uint32_t outer_off = db_byte_offset_at(&db, fid, /*line=*/6, /*col=*/4);
  if (outer_off == UINT32_MAX) {
    fprintf(stderr, "FAIL: couldn't compute outer-use byte offset\n");
    ok = 0;
  }
  SyntaxNode *outer_use = outer_off != UINT32_MAX
                              ? db_node_at_offset(&db, fid, outer_off)
                              : NULL;
  if (!outer_use) {
    fprintf(stderr, "FAIL: no SyntaxNode at outer-use position (line 6, col 4)\n");
    ok = 0;
  } else {
    bool found = false;
    IpIndex outer_type =
        sema_body_scope_lookup(&db, defA, outer_use, y_name, &found);
    if (!found) {
      fprintf(stderr, "FAIL: outer-use lookup didn't find 'y'\n");
      ok = 0;
    } else if (outer_type.v != IP_F32_TYPE.v) {
      fprintf(stderr,
              "FAIL: outer-use 'y' type = %u, want IP_F32_TYPE=%u "
              "(outer bind is `y := x` where x: f32)\n",
              outer_type.v, IP_F32_TYPE.v);
      ok = 0;
    }
    syntax_node_release(outer_use);
  }

  db_request_end(&db);
  db_free(&db);

  if (ok) {
    printf("PASS scope_shadowing_lookup: inner `y` → comptime_int; "
           "outer `y` → f32\n");
    return 0;
  }
  printf("FAIL scope_shadowing_lookup\n");
  return 1;
}
