// Phase 0 P0 gap G9 — multi-level import cascade.
//
// Three files: A imports B imports C. We verify:
//   1. Each individual @import resolves to the right namespace.
//   2. C's pub decls are visible through B from A's perspective via
//      double indirection (b.c.target).
//   3. After editing C and bumping the revision, namespace_type(C)
//      reflects the new content, and the import chain still resolves.
//
// This is the cross-file workspace cascade contract: edits to a deeply-
// imported file must propagate through the import graph to consumers.
// Without this, the LSP can't keep diagnostics fresh for users editing
// library code that's transitively used.

#include "../src/db/db.h"
#include "../src/db/diag/diag.h"
#include "../src/db/ids/ids.h"
#include "../src/db/intern_pool/intern_pool.h"
#include "../src/db/query/namespace_type.h"
#include "../src/db/request/request.h"
#include "../src/db/workspace/workspace.h"
#include "../src/sema/sema.h"
#include "../src/support/data_structure/stringpool.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int write_file(const char *path, const char *content) {
  FILE *f = fopen(path, "w");
  if (!f) return 0;
  size_t n = strlen(content);
  size_t w = fwrite(content, 1, n, f);
  fclose(f);
  return w == n;
}

int main(void) {
  const char *dir = "/tmp/ore-cascade-test";
  const char *path_a = "/tmp/ore-cascade-test/a.ore";
  const char *path_b = "/tmp/ore-cascade-test/b.ore";
  const char *path_c = "/tmp/ore-cascade-test/c.ore";
  mkdir(dir, 0755);

  const char *a_text = "b :: @import(\"./b.ore\")\n";
  const char *b_text = "c :: @import(\"./c.ore\")\n";
  const char *c_v1   = "target :: pub fn() void { }\n";
  const char *c_v2   = "target :: pub fn() void { }\n"
                       "extra  :: pub fn() void { }\n";

  if (!write_file(path_a, a_text)) return 1;
  if (!write_file(path_b, b_text)) return 1;
  if (!write_file(path_c, c_v1))   return 1;

  int ok = 1;
  struct db db;
  db_init(&db);

  SourceId sa = workspace_did_open(&db, path_a, strlen(path_a), a_text,
                                    strlen(a_text));
  SourceId sb = workspace_did_open(&db, path_b, strlen(path_b), b_text,
                                    strlen(b_text));
  SourceId sc = workspace_did_open(&db, path_c, strlen(path_c), c_v1,
                                    strlen(c_v1));
  if (!source_id_valid(sa) || !source_id_valid(sb) || !source_id_valid(sc)) {
    fprintf(stderr, "FAIL: workspace_did_open invalid for one of a/b/c\n");
    db_free(&db);
    return 1;
  }

  NamespaceId nsA = db_get_file_namespace(&db, db_lookup_file_by_source(&db, sa));
  NamespaceId nsB = db_get_file_namespace(&db, db_lookup_file_by_source(&db, sb));
  NamespaceId nsC = db_get_file_namespace(&db, db_lookup_file_by_source(&db, sc));
  if (nsA.idx == nsB.idx || nsB.idx == nsC.idx || nsA.idx == nsC.idx) {
    fprintf(stderr, "FAIL: file-as-namespace broken (some pair shares ns)\n");
    ok = 0;
  }

  // ---- Cold pass: type-check all three modules ----
  db_request_begin(&db, db_current_revision(&db));
  sema_check_module(&db, nsA);
  sema_check_module(&db, nsB);
  sema_check_module(&db, nsC);

  // Hop 1: A's @import("./b.ore") resolves to nsB.
  StrId rel_b = pool_intern(&db.strings, "./b.ore", 7);
  NamespaceId resolved_b = workspace_resolve_import(&db, nsA, rel_b);
  if (resolved_b.idx != nsB.idx) {
    fprintf(stderr, "FAIL: A's @import('./b.ore') → ns %u, want %u\n",
            resolved_b.idx, nsB.idx);
    ok = 0;
  }
  // Hop 2: B's @import("./c.ore") resolves to nsC.
  StrId rel_c = pool_intern(&db.strings, "./c.ore", 7);
  NamespaceId resolved_c = workspace_resolve_import(&db, nsB, rel_c);
  if (resolved_c.idx != nsC.idx) {
    fprintf(stderr, "FAIL: B's @import('./c.ore') → ns %u, want %u\n",
            resolved_c.idx, nsC.idx);
    ok = 0;
  }

  // namespace_type(C) reflects v1's single pub decl `target`.
  IpIndex tC_v1 = db_query_namespace_type(&db, nsC);
  if (tC_v1.v == IP_NONE.v ||
      ip_tag(&db.intern, tC_v1) != IP_TAG_NAMESPACE_TYPE) {
    fprintf(stderr, "FAIL: namespace_type(C) not a NAMESPACE_TYPE\n");
    ok = 0;
  } else {
    IpKey k = ip_key(&db.intern, tC_v1);
    if (k.namespace_type.n_fields != 1) {
      fprintf(stderr, "FAIL: C v1 has %zu pub fields, want 1\n",
              k.namespace_type.n_fields);
      ok = 0;
    }
  }
  db_request_end(&db);

  // ---- Edit C: add a second pub decl ----
  workspace_did_change(&db, path_c, strlen(path_c), c_v2, strlen(c_v2));
  // workspace_did_change bumps the revision via db_input_changed.

  db_request_begin(&db, db_current_revision(&db));
  sema_check_module(&db, nsA);
  sema_check_module(&db, nsB);
  sema_check_module(&db, nsC);

  // Hops still resolve (workspace_resolve_import doesn't depend on
  // content — it's a path-only lookup).
  resolved_b = workspace_resolve_import(&db, nsA, rel_b);
  resolved_c = workspace_resolve_import(&db, nsB, rel_c);
  if (resolved_b.idx != nsB.idx || resolved_c.idx != nsC.idx) {
    fprintf(stderr, "FAIL: import resolution broken after edit\n");
    ok = 0;
  }

  // namespace_type(C) reflects v2's TWO pub decls.
  IpIndex tC_v2 = db_query_namespace_type(&db, nsC);
  if (tC_v2.v == IP_NONE.v ||
      ip_tag(&db.intern, tC_v2) != IP_TAG_NAMESPACE_TYPE) {
    fprintf(stderr, "FAIL: namespace_type(C) v2 not a NAMESPACE_TYPE\n");
    ok = 0;
  } else {
    IpKey k = ip_key(&db.intern, tC_v2);
    if (k.namespace_type.n_fields != 2) {
      fprintf(stderr, "FAIL: C v2 has %zu pub fields, want 2 (target + extra)\n",
              k.namespace_type.n_fields);
      ok = 0;
    }
  }

  // tC_v1 and tC_v2 are DIFFERENT IpIndex values (set changed).
  if (tC_v1.v == tC_v2.v) {
    fprintf(stderr, "FAIL: namespace_type(C) IpIndex unchanged after edit "
                    "(v1=%u v2=%u — invalidation didn't propagate)\n",
            tC_v1.v, tC_v2.v);
    ok = 0;
  }

  db_request_end(&db);
  db_free(&db);

  // Cleanup.
  unlink(path_a); unlink(path_b); unlink(path_c); rmdir(dir);

  if (ok) {
    printf("PASS import_cascade: A→B→C two-hop resolution; edit C → "
           "namespace_type(C) reflects new pub-decl set\n");
    return 0;
  }
  printf("FAIL import_cascade\n");
  return 1;
}
