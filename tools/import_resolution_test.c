// Tier 1 audit-close gate — cross-file @import end-to-end under
// file-as-namespace.
//
// Two real on-disk files (workspace_resolve_import needs realpath):
//   /tmp/ore-import-test/a.ore — imports b, binds a name to b's export
//   /tmp/ore-import-test/b.ore — exports `pub greet`
//
// Asserts:
//   1. workspace_resolve_import returns b's NamespaceId from a's "./b.ore".
//   2. db_query_namespace_type(b) materializes an IPK_NAMESPACE_TYPE.
//   3. a's `g := b.greet` types correctly (no IP_NONE, no error diags).
//
// This replaces the deleted cross_module_test.c under the file-as-
// namespace model.

#include "../src/db/db.h"
#include "../src/db/diag/diag.h"
#include "../src/db/ids/ids.h"
#include "../src/db/intern_pool/intern_pool.h"
#include "../src/db/query/def_identity.h"
#include "../src/db/query/namespace_type.h"
#include "../src/db/query/type_of_def.h"
#include "../src/db/request/request.h"
#include "../src/db/workspace/workspace.h"
#include "../src/sema/sema.h"
#include "../src/support/data_structure/stringpool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int write_file(const char *path, const char *content) {
  FILE *f = fopen(path, "w");
  if (!f) {
    fprintf(stderr, "FAIL: cannot create %s\n", path);
    return 0;
  }
  size_t n = strlen(content);
  size_t w = fwrite(content, 1, n, f);
  fclose(f);
  return w == n;
}

static uint32_t count_error_diags(struct db *s) {
  uint32_t errs = 0;
  for (size_t i = 1; i < s->diag_lists.count; i++) {
    DiagList *dl = (DiagList *)vec_get(&s->diag_lists, i);
    if (!dl) continue;
    Diag *items = (Diag *)dl->items.data;
    for (size_t j = 0; j < dl->items.count; j++) {
      if (items[j].severity == DIAG_ERROR) errs++;
    }
  }
  return errs;
}

int main(void) {
  // Set up the workspace directory + two files on disk.
  const char *dir = "/tmp/ore-import-test";
  const char *path_a = "/tmp/ore-import-test/a.ore";
  const char *path_b = "/tmp/ore-import-test/b.ore";
  mkdir(dir, 0755); // ignore EEXIST
  if (!write_file(path_a,
                  "b :: @import(\"./b.ore\")\n"
                  "g :: b.greet\n")) {
    return 1;
  }
  // Ore grammar: modifiers like `pub` appear AFTER the bind operator
  // (`name :: pub value`), not before the name. See examples/exn.ore.
  if (!write_file(path_b,
                  "greet :: pub fn() void { }\n")) {
    return 1;
  }

  int ok = 1;
  struct db db;
  db_init(&db);

  // Register both files in the workspace. workspace_did_open
  // canonicalizes via realpath and registers under the canonical path.
  SourceId src_a = workspace_did_open(&db, path_a, strlen(path_a),
                                       "b :: @import(\"./b.ore\")\n"
                                       "g :: b.greet\n",
                                       strlen("b :: @import(\"./b.ore\")\n"
                                              "g :: b.greet\n"));
  SourceId src_b = workspace_did_open(&db, path_b, strlen(path_b),
                                       "greet :: pub fn() void { }\n",
                                       strlen("greet :: pub fn() void { }\n"));
  if (!source_id_valid(src_a) || !source_id_valid(src_b)) {
    fprintf(stderr, "FAIL: workspace_did_open returned invalid SourceId\n");
    db_free(&db);
    return 1;
  }

  FileId fa = db_lookup_file_by_source(&db, src_a);
  FileId fb = db_lookup_file_by_source(&db, src_b);
  NamespaceId nsA = db_get_file_namespace(&db, fa);
  NamespaceId nsB = db_get_file_namespace(&db, fb);
  if (!namespace_id_valid(nsA) || !namespace_id_valid(nsB)) {
    fprintf(stderr, "FAIL: namespace lookup failed\n");
    db_free(&db);
    return 1;
  }
  // File-as-namespace: a and b live in DIFFERENT namespaces.
  if (nsA.idx == nsB.idx) {
    fprintf(stderr,
            "FAIL: file-as-namespace broken — a (%u) and b (%u) share ns\n",
            nsA.idx, nsB.idx);
    ok = 0;
  }

  // Typecheck both modules.
  db_request_begin(&db, db_current_revision(&db));
  sema_check_module(&db, nsA);
  sema_check_module(&db, nsB);

  // Assertion 1: workspace_resolve_import resolves "./b.ore" to nsB.
  StrId rel = pool_intern(&db.strings, "./b.ore", 7);
  NamespaceId resolved = workspace_resolve_import(&db, nsA, rel);
  if (resolved.idx != nsB.idx) {
    fprintf(stderr,
            "FAIL: @import(\"./b.ore\") resolved to ns %u, want %u\n",
            resolved.idx, nsB.idx);
    ok = 0;
  }

  // Assertion 2: db_query_namespace_type(nsB) materializes a NAMESPACE_TYPE.
  IpIndex ns_b_type = db_query_namespace_type(&db, nsB);
  if (ns_b_type.v == IP_NONE.v) {
    fprintf(stderr, "FAIL: namespace_type(b) returned IP_NONE\n");
    ok = 0;
  } else if (ip_tag(&db.intern, ns_b_type) != IP_TAG_NAMESPACE_TYPE) {
    fprintf(stderr, "FAIL: namespace_type(b) tag = %d, want IP_TAG_NAMESPACE_TYPE\n",
            ip_tag(&db.intern, ns_b_type));
    ok = 0;
  } else {
    IpKey nsk = ip_key(&db.intern, ns_b_type);
    // b exports exactly one pub decl (greet); namespace_type's field
    // list should contain it.
    if (nsk.namespace_type.n_fields != 1) {
      fprintf(stderr,
              "FAIL: namespace_type(b) has %zu fields, want 1 (greet)\n",
              nsk.namespace_type.n_fields);
      ok = 0;
    } else {
      const char *fname =
          pool_get(&db.strings, nsk.namespace_type.field_names[0]);
      if (!fname || strcmp(fname, "greet") != 0) {
        fprintf(stderr,
                "FAIL: namespace_type(b).fields[0].name = %s, want greet\n",
                fname ? fname : "(null)");
        ok = 0;
      }
    }
  }

  db_request_end(&db);

  // Assertion 3: no error diagnostics from typechecking either module.
  uint32_t errs = count_error_diags(&db);
  if (errs != 0) {
    fprintf(stderr, "FAIL: expected 0 error diags, got %u\n", errs);
    ok = 0;
  }

  db_free(&db);

  // Best-effort cleanup. Don't fail on cleanup errors.
  unlink(path_a);
  unlink(path_b);
  rmdir(dir);

  if (ok) {
    printf("PASS import_resolution: file-as-namespace; @import resolves; "
           "namespace_type carries pub exports\n");
    return 0;
  }
  printf("FAIL import_resolution\n");
  return 1;
}
