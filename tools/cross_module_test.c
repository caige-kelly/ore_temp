// Gap B gate — multi-file modules + @import resolution.
//
// Two files in the same directory (a.ore + b.ore) registered through
// workspace_did_open. Asserts:
//
//   1. directory-as-module: db_module_for_directory pins them to the
//      same ModuleId.
//   2. @import("./b.ore") in a.ore resolves to that same module
//      (db_query_module_for_path returns the right id).
//   3. sema typechecks both files end-to-end with no error diagnostics
//      (the @import expression types as IP_TAG_NAMESPACE).
//   4. Cross-file incrementality: editing a.ore's body leaves b.ore's
//      sema slots FROZEN (Gap A holds across files because each decl
//      has its own QUERY_DECL_AST fingerprint).
//   5. Cross-file dep propagation: editing b.ore's exported decl makes
//      a.ore's consumer queries see a new dep fingerprint and recompute.
//
// The fixture lives entirely in memory — no disk I/O — so the test is
// hermetic and CI-friendly.

#include "../src/db/db.h"
#include "../src/db/diag/diag.h"
#include "../src/db/ids/ids.h"
#include "../src/db/intern_pool/intern_pool.h"
#include "../src/db/query/ast.h"
#include "../src/db/query/def_identity.h"
#include "../src/db/query/invalidate.h"
#include "../src/db/query/module_for_path.h"
#include "../src/db/query/query.h"
#include "../src/db/request/request.h"
#include "../src/db/storage/stringpool.h"
#include "../src/db/workspace/workspace.h"
#include "../src/sema/sema.h"

#include <stdio.h>
#include <string.h>

// computed_rev of a DefId-keyed sema slot (cold column).
static uint64_t sema_rev(struct db *s, QueryKind kind, DefId def) {
  QuerySlotCold *sl = db_locate_slot_cold(s, kind, (uint64_t)def.idx);
  return sl ? sl->computed_rev : 0;
}

// Count error-severity diagnostics across the whole db, optionally
// printing them via the canonical formatter for debugging.
static uint32_t count_errors(struct db *s, bool print) {
  uint32_t errs = 0;
  for (size_t i = 1; i < s->diag_lists.count; i++) {
    DiagList *dl = (DiagList *)vec_get(&s->diag_lists, i);
    if (!dl)
      continue;
    Diag *items = (Diag *)dl->items.data;
    for (size_t j = 0; j < dl->items.count; j++) {
      if (items[j].severity == DIAG_ERROR) {
        errs++;
        if (print) {
          fprintf(stderr, "  diag: ");
          db_print_diag(s, &items[j], stderr);
          fprintf(stderr, "\n");
        }
      }
    }
  }
  return errs;
}

int main(void) {
  struct db db;
  db_init(&db);

  const char *path_a = "/tmp/gapb/a.ore";
  const char *path_b = "/tmp/gapb/b.ore";

  // Initial fixture.
  //   b.ore exports `greet`.
  //   a.ore imports b and binds `g` to b.greet — exercises
  //   module-member access (AST_EXPR_FIELD on an IP_TAG_NAMESPACE).
  //   `noop` is a placeholder a-side body we'll edit in assertion 4
  //   (without changing a.ore's top-level decl set, so
  //   QUERY_TOP_LEVEL_INDEX stays stable).
  const char *a1 = "b :: @import(\"./b.ore\")\n"
                   "g :: b.greet\n"
                   "noop :: fn() i32\n"
                   "    1\n";
  const char *b1 = "greet :: fn() void\n"
                   "    return\n";

  workspace_did_open(&db, path_a, strlen(path_a), a1, strlen(a1));
  workspace_did_open(&db, path_b, strlen(path_b), b1, strlen(b1));

  int ok = 1;

  // ---- Assertion 1: directory-as-module ----
  SourceId src_a = db_lookup_source_by_path(&db, path_a, strlen(path_a));
  SourceId src_b = db_lookup_source_by_path(&db, path_b, strlen(path_b));
  FileId fa = db_lookup_file_by_source(&db, src_a);
  FileId fb = db_lookup_file_by_source(&db, src_b);
  ModuleId ma = db_get_file_module(&db, fa);
  ModuleId mb = db_get_file_module(&db, fb);
  if (ma.idx != mb.idx) {
    fprintf(stderr,
            "FAIL: a.ore (module %u) and b.ore (module %u) should share a "
            "module — directory-as-module policy not honored by "
            "workspace_did_open\n",
            ma.idx, mb.idx);
    ok = 0;
  }
  ModuleId M = ma;

  // ---- Sema (rev 1). ----
  uint64_t rev1 = db_current_revision(&db);
  db_request_begin(&db, rev1);
  sema_check_module(&db, M);

  // ---- Assertion 2: @import("./b.ore") resolves to M itself. ----
  StrId rel = pool_intern(&db.strings, "./b.ore", 7);
  ModuleId resolved = db_query_module_for_path(&db, M, rel);
  if (resolved.idx != M.idx) {
    fprintf(stderr,
            "FAIL: @import(\"./b.ore\") resolved to module %u, expected %u\n",
            resolved.idx, M.idx);
    ok = 0;
  }
  db_request_end(&db);

  // ---- Assertion 3: no error diagnostics from sema. ----
  uint32_t errs = count_errors(&db, /*print=*/true);
  if (errs != 0) {
    fprintf(stderr,
            "FAIL: expected 0 error diagnostics after rev1 sema, got %u\n",
            errs);
    ok = 0;
  }

  // Snapshot greet's DefId + sema-slot revs so the next two edits can
  // assert frozen-vs-recomputed.
  FileArray *tli_b =
      (FileArray *)vec_get(&db.files.top_level_indices, file_id_local(fb));
  if (!tli_b || tli_b->count < 1) {
    fprintf(stderr, "FAIL: b.ore must have at least one top-level decl\n");
    db_request_end(&db); // defensive
    db_free(&db);
    printf("FAIL cross_module\n");
    return 1;
  }
  AstId ast_greet = ((TopLevelEntry *)tli_b->data)[0].ast_id;

  db_request_begin(&db, db_current_revision(&db));
  DefId def_greet = db_query_def_identity(&db, M, ast_greet);
  db_request_end(&db);

  uint64_t greet_rev1 = sema_rev(&db, QUERY_TYPE_OF_DECL, def_greet);

  // ---- Assertion 4: edit a.ore's body → b.ore's sema slots FROZEN. ----
  // Only `noop`'s body changes — top-level decl set is unchanged, so
  // QUERY_TOP_LEVEL_INDEX(M)'s fingerprint stays the same, so b's
  // decls' verify-deps don't see a changed fingerprint, and greet's
  // sema slots stay at rev1.
  const char *a2 = "b :: @import(\"./b.ore\")\n"
                   "g :: b.greet\n"
                   "noop :: fn() -> i32\n"
                   "    2\n";
  if (!db_set_source_text(&db, src_a, a2, strlen(a2))) {
    fprintf(stderr, "FAIL: a-edit reported no change\n");
    ok = 0;
  }
  uint64_t rev2 = db_current_revision(&db);
  db_request_begin(&db, rev2);
  sema_check_module(&db, M);
  db_request_end(&db);

  uint64_t greet_rev2 = sema_rev(&db, QUERY_TYPE_OF_DECL, def_greet);
  if (greet_rev2 != greet_rev1) {
    fprintf(stderr,
            "FAIL: edit to a.ore re-typechecked greet in b.ore "
            "(computed_rev %llu -> %llu) — cross-file Gap A broken\n",
            (unsigned long long)greet_rev1, (unsigned long long)greet_rev2);
    ok = 0;
  }

  // ---- Assertion 5: edit b.ore's signature → a.ore re-typechecks. ----
  // Rename greet's parameter list (currently empty; add a param). Its
  // QUERY_DECL_AST fingerprint changes; consumers in a.ore that read
  // greet's type via b.greet must recompute.
  const char *b2 = "pub greet :: fn(x: i32) void\n";
  if (!db_set_source_text(&db, src_b, b2, strlen(b2))) {
    fprintf(stderr, "FAIL: b-edit reported no change\n");
    ok = 0;
  }
  uint64_t rev3 = db_current_revision(&db);
  db_request_begin(&db, rev3);
  sema_check_module(&db, M);
  db_request_end(&db);

  uint64_t greet_rev3 = sema_rev(&db, QUERY_TYPE_OF_DECL, def_greet);
  if (greet_rev3 == greet_rev2) {
    fprintf(stderr,
            "FAIL: edit to b.ore's exported decl did NOT trigger greet's "
            "sema recompute (computed_rev still %llu) — cross-file dep "
            "propagation broken\n",
            (unsigned long long)greet_rev3);
    ok = 0;
  }

  db_free(&db);
  if (ok) {
    printf("PASS cross_module: same-dir → same module; @import resolves; "
           "a-edit leaves b frozen; b-edit re-typechecks consumers\n");
    return 0;
  }
  printf("FAIL cross_module\n");
  return 1;
}
