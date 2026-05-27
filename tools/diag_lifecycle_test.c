// Phase 0 P0 gap G10 — diag slot lifecycle.
//
// KNOWN FAILING TEST. Documents an architectural bug discovered during
// Phase 0. Must pass after the rewrite. NOT part of the green test gate
// today.
//
// Bug: when an edit shifts a decl's byte range, the new parse produces
// a different SyntaxNodePtr for the same logical decl, which routes to
// a new DefId. The old DefId's slot stays in QUERY_DONE forever — and
// so do its diagnostics. db_collect_diags_for_file still walks them.
//
// Symptom: edit fixes a typo → user expects the error to vanish → the
// error stays on screen because it's attached to the orphan DefId.
//
// Engine contract that the rewrite must satisfy:
//   After an edit that removes a previously-emitting decl (or shifts
//   its range so a new DefId is allocated), db_collect_diags_for_file
//   must NOT return diagnostics anchored to orphan DefIds.
//
// Architectural fix path (from Phase 0 plan):
//   file_ast push-stamps each top-level entry's slot with the current
//   revision. Diag collection filters out diag units whose owning
//   slot's verified_rev < current_rev — those units belong to orphan
//   DefIds and shouldn't be reported.

#include "../src/db/db.h"
#include "../src/db/diag/diag.h"
#include "../src/db/ids/ids.h"
#include "../src/db/query/query.h"
#include "../src/db/request/request.h"
#include "../src/sema/sema.h"
#include "../src/support/data_structure/vec.h"

#include <stdio.h>
#include <string.h>

extern Fingerprint db_query_top_level_index(struct db *s, NamespaceId mod);

static uint32_t count_error_diags(struct db *s, FileId fid) {
  Vec diags;
  vec_init(&diags, sizeof(Diag));
  db_collect_diags_for_file(s, fid, &diags);
  uint32_t errs = 0;
  for (size_t i = 0; i < diags.count; i++) {
    Diag *d = (Diag *)vec_get(&diags, i);
    if (d->severity == DIAG_ERROR) errs++;
  }
  vec_free(&diags);
  return errs;
}

int main(void) {
  struct db db;
  db_init(&db);

  const char *path = "diag_life.ore";

  // v1: A's body references undefined `undef_name` → undefined-id diag.
  const char *v1 = "A :: fn() void { undef_name }\n";
  // v2: completely empty body. No references, should emit no diags.
  // v2 has a DIFFERENT byte length than v1 → A's SyntaxNodePtr (and
  // therefore DefId) is different in v2. v1's DefId is now orphan.
  const char *v2 = "A :: fn() void { }\n";

  SourceId src = db_create_source(&db, path, strlen(path), v1, strlen(v1));
  NamespaceId M = db_create_namespace(&db);
  FileId fid = db_create_file(&db, src, M);

  // ---- v1: typecheck, expect 1 error ----
  db_request_begin(&db, db_current_revision(&db));
  (void)db_query_top_level_index(&db, M);
  sema_check_module(&db, M);
  db_request_end(&db);

  uint32_t v1_errors = count_error_diags(&db, fid);
  if (v1_errors == 0) {
    fprintf(stderr,
            "FAIL: v1 (with `undef_name` reference) emitted no error "
            "diagnostics — fixture or sema is broken\n");
    db_free(&db);
    printf("FAIL diag_lifecycle\n");
    return 1;
  }

  // ---- Edit to v2 (clean body) ----
  if (!db_set_source_text(&db, src, v2, strlen(v2))) {
    fprintf(stderr, "FAIL: db_set_source_text returned false\n");
    db_free(&db);
    printf("FAIL diag_lifecycle\n");
    return 1;
  }

  db_request_begin(&db, db_current_revision(&db));
  (void)db_query_top_level_index(&db, M);
  sema_check_module(&db, M);
  db_request_end(&db);

  // ---- After v2: collected diags must be 0 (no errors in v2's code) ----
  // THIS IS THE FAILING ASSERTION — orphan DefId still has its diag.
  uint32_t v2_errors = count_error_diags(&db, fid);
  if (v2_errors != 0) {
    fprintf(stderr,
            "FAIL: after editing to a clean fn body, db_collect_diags_for_file "
            "returned %u errors. Expected 0.\n"
            "  Root cause: v1's DefId is orphan (different SyntaxNodePtr "
            "due to byte-range shift). Its diag persists in db.diag_lists\n"
            "  and is still collected because the collector doesn't filter\n"
            "  by current slot liveness.\n"
            "  Rewrite must fix via push-stamp liveness + filtered collect.\n",
            v2_errors);
    db_free(&db);
    printf("FAIL diag_lifecycle (known orphan-DefId bug)\n");
    return 1;
  }

  db_free(&db);
  printf("PASS diag_lifecycle: edit-then-fix clears stale diagnostic\n");
  return 0;
}
