// Pre-Phase-C debt D-B2 (Gap G8) — import cycle safety.
//
// Locks in that mutually- and self-referential @imports resolve without
// unbounded recursion / OOM / hang.
//
// Finding from the pre-Phase-C audit: workspace_resolve_import is
// ITERATIVE, not recursive — it registers at most one file per call,
// and its registry-idempotency check (db_lookup_source_by_path) means a
// back-edge resolves to the already-registered NamespaceId rather than
// re-loading. So cycles are safe BY CONSTRUCTION. This test is the
// regression guard that keeps it that way — if a future change makes
// resolution recurse without a guard, this test hangs (caught by the
// wall-clock guard the Makefile target wraps it in).
//
// Cases:
//   1. A imports B, B imports A   → both directions resolve to the peer.
//   2. C imports C (self-import)  → resolves to C itself.
//   3. A→B→C→A (3-cycle)          → each hop resolves; closing edge
//                                    resolves to the origin.

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/workspace/workspace.h"
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

static NamespaceId ns_of(struct db *s, SourceId src) {
  FileId fid = db_lookup_file_by_source(s, src);
  return db_get_file_namespace(s, fid);
}

int main(void) {
  const char *dir = "/tmp/ore-cycle-test";
  const char *pa = "/tmp/ore-cycle-test/a.ore";
  const char *pb = "/tmp/ore-cycle-test/b.ore";
  const char *pc = "/tmp/ore-cycle-test/c.ore";
  const char *ps = "/tmp/ore-cycle-test/selfish.ore";
  mkdir(dir, 0755);

  // 2-cycle: a <-> b
  const char *a_txt = "b :: @import(\"./b.ore\")\n";
  const char *b_txt = "a :: @import(\"./a.ore\")\n";
  // self-import
  const char *s_txt = "me :: @import(\"./selfish.ore\")\n";
  // 3-cycle uses the same files plus c
  const char *c_txt = "a :: @import(\"./a.ore\")\n";

  if (!write_file(pa, a_txt) || !write_file(pb, b_txt) ||
      !write_file(pc, c_txt) || !write_file(ps, s_txt)) {
    fprintf(stderr, "FAIL: could not write fixtures\n");
    return 1;
  }

  int ok = 1;
  struct db db;
  db_init(&db);

  SourceId sa = workspace_did_open(&db, pa, strlen(pa), a_txt, strlen(a_txt));
  SourceId sb = workspace_did_open(&db, pb, strlen(pb), b_txt, strlen(b_txt));
  SourceId ss = workspace_did_open(&db, ps, strlen(ps), s_txt, strlen(s_txt));
  if (!source_id_valid(sa) || !source_id_valid(sb) || !source_id_valid(ss)) {
    fprintf(stderr, "FAIL: workspace_did_open invalid\n");
    db_free(&db);
    return 1;
  }

  NamespaceId nsA = ns_of(&db, sa);
  NamespaceId nsB = ns_of(&db, sb);
  NamespaceId nsS = ns_of(&db, ss);

  StrId to_b = pool_intern(&db.strings, "./b.ore", 7);
  StrId to_a = pool_intern(&db.strings, "./a.ore", 7);
  StrId to_self = pool_intern(&db.strings, "./selfish.ore", 13);

  // ---- Case 1: A↔B ----
  NamespaceId rb = workspace_resolve_import(&db, nsA, to_b);
  if (rb.idx != nsB.idx) {
    fprintf(stderr, "FAIL: A's import of ./b.ore → %u, want %u\n", rb.idx, nsB.idx);
    ok = 0;
  }
  // The closing edge: B imports A. A is already registered → must
  // resolve to A's existing nsid (NOT recurse / re-load / OOM).
  NamespaceId ra = workspace_resolve_import(&db, nsB, to_a);
  if (ra.idx != nsA.idx) {
    fprintf(stderr, "FAIL: B's import of ./a.ore → %u, want %u (cycle close)\n",
            ra.idx, nsA.idx);
    ok = 0;
  }

  // ---- Case 2: self-import ----
  NamespaceId rs = workspace_resolve_import(&db, nsS, to_self);
  if (rs.idx != nsS.idx) {
    fprintf(stderr, "FAIL: self-import → %u, want %u\n", rs.idx, nsS.idx);
    ok = 0;
  }

  // ---- Case 3: 3-cycle A→B→C→A ----
  // C is NOT yet registered (only a/b/selfish were did_open'd). Resolving
  // a fresh import of ./c.ore lazy-loads it. Then C imports ./a.ore which
  // is already registered → closes the 3-cycle without recursion.
  StrId to_c = pool_intern(&db.strings, "./c.ore", 7);
  NamespaceId rc = workspace_resolve_import(&db, nsB, to_c); // pretend B also imports C
  if (!namespace_id_valid(rc)) {
    fprintf(stderr, "FAIL: lazy-load of ./c.ore returned invalid ns\n");
    ok = 0;
  } else {
    NamespaceId rca = workspace_resolve_import(&db, rc, to_a);
    if (rca.idx != nsA.idx) {
      fprintf(stderr, "FAIL: C's import of ./a.ore → %u, want %u (3-cycle close)\n",
              rca.idx, nsA.idx);
      ok = 0;
    }
  }

  db_free(&db);
  unlink(pa); unlink(pb); unlink(pc); unlink(ps); rmdir(dir);

  if (ok) {
    printf("PASS import_cycle: A<->B, self-import, and A->B->C->A all "
           "resolve via registry idempotency; no recursion\n");
    return 0;
  }
  printf("FAIL import_cycle\n");
  return 1;
}
