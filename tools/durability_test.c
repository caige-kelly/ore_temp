// Step 4 gate — durability fast-path.
//
// Two modules: LIB (one file from a HIGH/library source) and W (one
// file from a LOW/workspace source). Build both module indexes, then
// edit ONLY the LOW workspace file and re-query both indexes.
//
// Assertions:
//   - W's file + index recompute (its LOW tier changed).
//   - LIB's file + index do NOT recompute (computed_rev frozen).
//   - DECISIVE: LIB's QUERY_FILE_AST slot.verified_rev stays at the
//     OLD revision. If the exact dep walk had run for LIB's index, it
//     would have recursed into that slot and bumped its verified_rev
//     to the new revision. It staying stale proves the durability
//     fast-path skipped the dependency walk entirely — which is the
//     whole point of the tiers.
//
// White-box pokes (no source-edit API in the db core — that's the
// out-of-scope LSP layer): replace the LOW source's bytes, reset its
// parse slot to QUERY_EMPTY, and signal the edit via db_input_changed
// (the real revision/durability bookkeeping entry point).

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/query/invalidate.h"
#include "../src/db/query/query.h"
#include "../src/db/request/request.h"

#include <stdio.h>
#include <string.h>

extern Fingerprint db_query_top_level_index(struct db *s, ModuleId mod);

static QuerySlot *file_slot(struct db *s, FileId fid) {
  return db_locate_slot(s, QUERY_FILE_AST, (uint64_t)fid.idx);
}
static QuerySlot *index_slot(struct db *s, ModuleId mid) {
  return db_locate_slot(s, QUERY_TOP_LEVEL_INDEX, (uint64_t)mid.idx);
}

int main(void) {
  struct db db;
  db_init(&db);

  // LIB — a library source (HIGH durability).
  const char *lp = "lib.ore", *lt = "Lib :: 100\n";
  SourceId ls = db_create_source(&db, lp, strlen(lp), lt, strlen(lt));
  db_set_source_durability(&db, ls, DUR_HIGH);
  ModuleId LIB = db_create_module(&db);
  FileId lf = db_create_file(&db, ls, LIB);
  db_add_file_to_module(&db, LIB, lf);

  // W — a workspace source (LOW durability, the default).
  const char *wp = "w.ore", *wt1 = "W :: 1\n";
  const char *wt2 = "W :: 2\nX :: 3\n"; // the edit
  SourceId ws = db_create_source(&db, wp, strlen(wp), wt1, strlen(wt1));
  ModuleId W = db_create_module(&db);
  FileId wf = db_create_file(&db, ws, W);
  db_add_file_to_module(&db, W, wf);

  // ---- Request 1 (revision 1). ----
  db_request_begin(&db, 1);
  db_query_top_level_index(&db, LIB);
  db_query_top_level_index(&db, W);
  db_request_end(&db);

  uint64_t lib_idx_c1 = index_slot(&db, LIB)->computed_rev;
  uint64_t lib_f_c1 = file_slot(&db, lf)->computed_rev;
  uint64_t w_idx_c1 = index_slot(&db, W)->computed_rev;

  int ok = 1;
  if (!(lib_idx_c1 == 1 && lib_f_c1 == 1 && w_idx_c1 == 1)) {
    fprintf(stderr, "FAIL: rev1 computed_rev lib_idx=%llu lib_f=%llu "
                    "w_idx=%llu (want 1,1,1)\n",
            (unsigned long long)lib_idx_c1, (unsigned long long)lib_f_c1,
            (unsigned long long)w_idx_c1);
    ok = 0;
  }

  // ---- Edit ONLY the LOW workspace file W, through the real API.
  // db_set_source_text reads W's tier (LOW) and bumps only that tier.
  db_set_source_text(&db, ws, wt2, strlen(wt2));
  uint64_t rev2 = db_current_revision(&db);

  // ---- Request 2 (revision 2). ----
  db_request_begin(&db, rev2);
  db_query_top_level_index(&db, LIB);
  db_query_top_level_index(&db, W);
  db_request_end(&db);

  uint64_t lib_idx_c2 = index_slot(&db, LIB)->computed_rev;
  uint64_t lib_f_c2 = file_slot(&db, lf)->computed_rev;
  uint64_t lib_f_v2 = file_slot(&db, lf)->verified_rev;
  uint64_t w_idx_c2 = index_slot(&db, W)->computed_rev;
  uint64_t w_f_c2 = file_slot(&db, wf)->computed_rev;

  if (w_f_c2 != rev2 || w_idx_c2 != rev2) {
    fprintf(stderr, "FAIL: W not recomputed (w_f=%llu w_idx=%llu want %llu)\n",
            (unsigned long long)w_f_c2, (unsigned long long)w_idx_c2,
            (unsigned long long)rev2);
    ok = 0;
  }
  if (lib_idx_c2 != 1 || lib_f_c2 != 1) {
    fprintf(stderr,
            "FAIL: LIB recomputed after unrelated LOW edit "
            "(lib_idx=%llu lib_f=%llu want 1,1)\n",
            (unsigned long long)lib_idx_c2, (unsigned long long)lib_f_c2);
    ok = 0;
  }
  // The decisive one: the fast-path skipped LIB index's dep walk, so
  // its HIGH dependency (lib file_ast) was never revalidated and its
  // verified_rev is still the OLD revision (1), not rev2.
  if (lib_f_v2 != 1) {
    fprintf(stderr,
            "FAIL: LIB's file dep was walked (verified_rev=%llu, want 1) "
            "— durability fast-path did NOT fire\n",
            (unsigned long long)lib_f_v2);
    ok = 0;
  }

  db_free(&db);

  if (ok) {
    printf("PASS durability: LOW edit -> W recomputed; HIGH-only LIB "
           "fast-path skipped (dep walk avoided)\n");
    return 0;
  }
  printf("FAIL durability\n");
  return 1;
}
