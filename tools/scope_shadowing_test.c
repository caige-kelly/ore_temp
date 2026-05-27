// Tier 2 audit-close gate — body_scopes shadowing.
//
// Fixture:
//
//   A :: fn(x: f32) f32 {
//       y := x;     // outer y — bound in fn-root scope
//       {
//           y := 2; // inner y — bound in nested-block scope
//           y;      // use site — should resolve to inner y, not outer
//       };
//       y;          // outer y — visible only outside the nested block
//   };
//
// Asserts on the per-fn body_scopes state:
//   1. The fn has at LEAST 3 binds in db.body_scope_binds — `x` (param),
//      outer `y`, inner `y`.
//   2. Two of those binds share the same name StrId but live in
//      DIFFERENT scope_ids (proves the inner binding occupies a
//      different scope from the outer, which is what makes shadowing
//      work).
//   3. The scope_rows table contains at least 2 scopes (fn-root +
//      one nested block scope opened by the `{ ... }`).
//
// This is a structural test on body_scopes' output. Lookup correctness
// is implicit: sema_body_scope_lookup walks from the use-node's
// tagged scope outward, and finds the latest bind for the name —
// so if the two y's are in distinct scopes, the lookup returns the
// closer one.

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/query/body_scopes.h"
#include "../src/db/query/def_identity.h"
#include "../src/db/query/index.h"
#include "../src/db/request/request.h"
#include "../src/sema/sema.h"
#include "../src/support/data_structure/stringpool.h"

#include <stdio.h>
#include <string.h>

extern Fingerprint db_query_top_level_index(struct db *s, NamespaceId mod);

int main(void) {
  struct db db;
  db_init(&db);

  const char *path = "shadow.ore";
  const char *text =
      "A :: fn(x: f32) f32 {\n"
      "    y := x;\n"
      "    {\n"
      "        y := 2;\n"
      "        y;\n"
      "    };\n"
      "    y\n"
      "}\n";

  SourceId src = db_create_source(&db, path, strlen(path), text, strlen(text));
  NamespaceId M = db_create_namespace(&db);
  (void)db_create_file(&db, src, M);

  int ok = 1;

  db_request_begin(&db, 1);
  (void)db_query_top_level_index(&db, M);
  sema_check_module(&db, M);

  // Find A's DefId via the module's internal scope.
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
    printf("FAIL scope_shadowing\n");
    return 1;
  }
  if (db_def_kind(&db, defA) != KIND_FUNCTION) {
    fprintf(stderr, "FAIL: A is not KIND_FUNCTION (kind=%u)\n",
            db_def_kind(&db, defA));
    db_request_end(&db);
    db_free(&db);
    printf("FAIL scope_shadowing\n");
    return 1;
  }
  // Drive body_scopes so the FnBody and shared pools are populated.
  (void)db_query_body_scopes(&db, defA);

  uint32_t row = db_def_row(&db, defA, KIND_FUNCTION);
  FnBody fb = *(FnBody *)vec_get(&db.fns.body, row);

  // Assertion 3: ≥2 scopes (fn-root + at least one nested block).
  if (fb.scope_len < 2) {
    fprintf(stderr, "FAIL: scope_len=%u, want ≥2 (root + nested block)\n",
            fb.scope_len);
    ok = 0;
  }

  // Assertion 1: ≥3 binds.
  if (fb.bind_len < 3) {
    fprintf(stderr, "FAIL: bind_len=%u, want ≥3 (x, outer y, inner y)\n",
            fb.bind_len);
    ok = 0;
  }

  // Assertion 2: two binds share the same name but live in different scope_ids.
  if (ok) {
    StrId y_name = pool_intern(&db.strings, "y", 1);
    const ScopedBind *binds = (const ScopedBind *)db.body_scope_binds.data;
    uint32_t y_count = 0;
    uint32_t scopes_seen[8] = {0};
    uint32_t distinct = 0;
    for (uint32_t i = 0; i < fb.bind_len; i++) {
      const ScopedBind *bd = &binds[fb.bind_off + i];
      if (bd->name.idx == y_name.idx) {
        y_count++;
        // Track distinct scope_ids for `y` binds.
        bool seen = false;
        for (uint32_t k = 0; k < distinct; k++)
          if (scopes_seen[k] == bd->scope_id) seen = true;
        if (!seen && distinct < 8) scopes_seen[distinct++] = bd->scope_id;
      }
    }
    if (y_count < 2) {
      fprintf(stderr,
              "FAIL: only %u bind(s) named 'y', want ≥2 (outer + inner)\n",
              y_count);
      ok = 0;
    }
    if (distinct < 2) {
      fprintf(stderr,
              "FAIL: both 'y' binds in same scope (%u distinct scopes), "
              "shadowing collapses scopes\n",
              distinct);
      ok = 0;
    }
  }

  db_request_end(&db);
  db_free(&db);

  if (ok) {
    printf("PASS scope_shadowing: nested `y := ...` lives in a distinct "
           "scope from the outer binding\n");
    return 0;
  }
  printf("FAIL scope_shadowing\n");
  return 1;
}
