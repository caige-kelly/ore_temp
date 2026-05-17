// Step 3 gate — proves per-file early-cutoff.
//
// Two files (A, B) in ONE module. Build the module's derived
// QUERY_TOP_LEVEL_INDEX (which aggregates over the file set, recording
// a dep on each file's QUERY_FILE_AST). Then "edit" file B only and
// re-query the index. Assert:
//   - file B's QUERY_FILE_AST recomputed (computed_rev advanced),
//   - file A's QUERY_FILE_AST was verified-unchanged and SKIPPED
//     (computed_rev frozen at the first revision) — the early-cutoff,
//   - the module index re-aggregated (computed_rev advanced) and its
//     fingerprint changed (B's content changed).
//
// The three white-box pokes mirror exactly what a real edit pipeline
// (the out-of-scope LSP layer) does, since the db core has no
// source-edit API yet: (1) replace B's source bytes, (2) reset B's
// parse slot to QUERY_EMPTY (precisely db_query_begin's own RECOMPUTE
// action), (3) advance the global current revision (the per-edit
// counter). The assertions observe the ENGINE's behavior, not faked
// results.

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/query/ast.h"
#include "../src/db/query/invalidate.h"
#include "../src/db/query/query.h"
#include "../src/db/request/request.h"

#include <stdio.h>
#include <string.h>

// Defined in src/db/query/index.c (no public header).
extern Fingerprint db_query_top_level_index(struct db *s, ModuleId mod);

static uint64_t file_computed_rev(struct db *s, FileId fid) {
  FileId *k = (FileId *)vec_get(&s->files.ids, file_id_local(fid));
  QuerySlot *sl = db_locate_slot(s, QUERY_FILE_AST, k);
  return sl ? sl->computed_rev : 0;
}

static uint64_t index_computed_rev(struct db *s, ModuleId mid) {
  ModuleId *k = (ModuleId *)vec_get(&s->modules.ids, mid.idx);
  QuerySlot *sl = db_locate_slot(s, QUERY_TOP_LEVEL_INDEX, k);
  return sl ? sl->computed_rev : 0;
}

int main(void) {
  struct db db;
  db_init(&db);

  const char *pa = "a.ore", *pb = "b.ore";
  const char *ta = "A :: 1\n";
  const char *tb1 = "B :: 2\n";
  const char *tb2 = "B :: 3\nC :: 4\n"; // the edit to B: adds top-level C

  SourceId sa = db_alloc_source(&db, pa, strlen(pa), ta, strlen(ta));
  SourceId sb = db_alloc_source(&db, pb, strlen(pb), tb1, strlen(tb1));

  ModuleId M = db_alloc_module(&db);
  FileId fa = db_alloc_file(&db, sa, M);
  db_module_add_file(&db, M, fa);
  FileId fb = db_alloc_file(&db, sb, M);
  db_module_add_file(&db, M, fb);

  // ---- Request 1 (revision 1): build the module index. ----
  db_request_begin(&db, 1);
  Fingerprint idx_fp1 = db_query_top_level_index(&db, M);
  db_request_end(&db);

  uint64_t a1 = file_computed_rev(&db, fa);
  uint64_t b1 = file_computed_rev(&db, fb);
  uint64_t i1 = index_computed_rev(&db, M);

  int ok = 1;
  if (!(a1 == 1 && b1 == 1 && i1 == 1)) {
    fprintf(stderr,
            "FAIL: rev1 computed_rev a=%llu b=%llu idx=%llu (want 1,1,1)\n",
            (unsigned long long)a1, (unsigned long long)b1,
            (unsigned long long)i1);
    ok = 0;
  }

  // ---- Edit file B only, through the real API. ----
  if (!db_source_set_text(&db, sb, tb2, strlen(tb2))) {
    fprintf(stderr, "FAIL: db_source_set_text reported no change\n");
    ok = 0;
  }
  uint64_t rev2 = db_current_revision(&db);

  // ---- Request 2: re-query the module index. ----
  db_request_begin(&db, rev2);
  Fingerprint idx_fp2 = db_query_top_level_index(&db, M);
  db_request_end(&db);

  uint64_t a2 = file_computed_rev(&db, fa);
  uint64_t b2 = file_computed_rev(&db, fb);
  uint64_t i2 = index_computed_rev(&db, M);

  if (a2 != 1) {
    fprintf(stderr,
            "FAIL: file A reparsed (computed_rev %llu, want 1 = early-cut)\n",
            (unsigned long long)a2);
    ok = 0;
  }
  if (b2 != 2) {
    fprintf(stderr, "FAIL: file B not reparsed (computed_rev %llu, want 2)\n",
            (unsigned long long)b2);
    ok = 0;
  }
  if (i2 != 2) {
    fprintf(stderr,
            "FAIL: module index not re-aggregated (computed_rev %llu, "
            "want 2)\n",
            (unsigned long long)i2);
    ok = 0;
  }
  if (idx_fp2 == idx_fp1) {
    fprintf(stderr,
            "FAIL: index fingerprint unchanged after B edit (%llu)\n",
            (unsigned long long)idx_fp2);
    ok = 0;
  }

  db_free(&db);

  if (ok) {
    printf("PASS file_incremental: edit B -> only B reparsed "
           "(A early-cut), module re-aggregated\n");
    return 0;
  }
  printf("FAIL file_incremental\n");
  return 1;
}
