// Production gate for db_set_source_text (the incremental source-edit
// primitive). Covers the three things that make it production-ready:
//
//   1. Byte-identical edit  -> no-op: returns false, version frozen,
//      revision frozen, no reparse (the hashes/versions fast path).
//   2. Real edit            -> returns true, version++, file reparses
//      (computed_rev advances), new text is what gets parsed.
//   3. Edit loop (N edits)  -> storage is BOUNDED. Source text is now
//      malloc-owned and freed on each set; this loop + db_free run
//      under ASan/LeakSanitizer and macOS `leaks` to prove the
//      arena->malloc migration leaks nothing across many edits.

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/query/invalidate.h"
#include "../src/db/query/query.h"
#include "../src/db/request/request.h"

#include <stdio.h>
#include <string.h>

extern Fingerprint db_query_file_ast(struct db *s, FileId fid);

// Only computed_rev is read off this — a COLD slot field.
static QuerySlotCold *fslot(struct db *s, FileId fid) {
  return db_locate_slot_cold(s, QUERY_FILE_AST, (uint64_t)fid.idx);
}
static uint32_t version_of(struct db *s, SourceId sid) {
  return *(uint32_t *)vec_get(&s->sources.versions, sid.idx);
}

int main(void) {
  struct db db;
  db_init(&db);

  const char *p = "e.ore";
  const char *t0 = "A :: 1\n";
  SourceId sid = db_create_source(&db, p, strlen(p), t0, strlen(t0));
  NamespaceId M = db_create_namespace(&db);
  FileId fid = db_create_file(&db, sid, M);

  db_request_begin(&db, 1);
  db_query_file_ast(&db, fid);
  db_request_end(&db);

  int ok = 1;
  uint64_t c0 = fslot(&db, fid)->computed_rev;
  uint32_t v0 = version_of(&db, sid);

  // 1. Byte-identical edit — must be a no-op.
  bool ch = db_set_source_text(&db, sid, t0, strlen(t0));
  if (ch) {
    fprintf(stderr, "FAIL: identical edit reported as changed\n");
    ok = 0;
  }
  if (version_of(&db, sid) != v0) {
    fprintf(stderr, "FAIL: identical edit bumped version\n");
    ok = 0;
  }
  db_request_begin(&db, db_current_revision(&db));
  db_query_file_ast(&db, fid);
  db_request_end(&db);
  if (fslot(&db, fid)->computed_rev != c0) {
    fprintf(stderr, "FAIL: identical edit triggered a reparse\n");
    ok = 0;
  }

  // 2. Real edit — must reparse and bump version.
  const char *t1 = "A :: 1\nB :: 2\n";
  ch = db_set_source_text(&db, sid, t1, strlen(t1));
  if (!ch) {
    fprintf(stderr, "FAIL: real edit reported as unchanged\n");
    ok = 0;
  }
  if (version_of(&db, sid) != v0 + 1) {
    fprintf(stderr, "FAIL: real edit did not bump version\n");
    ok = 0;
  }
  db_request_begin(&db, db_current_revision(&db));
  db_query_file_ast(&db, fid);
  db_request_end(&db);
  uint64_t c1 = fslot(&db, fid)->computed_rev;
  if (c1 <= c0) {
    fprintf(stderr, "FAIL: real edit did not reparse (computed_rev %llu)\n",
            (unsigned long long)c1);
    ok = 0;
  }

  // 3. Edit loop — storage must stay bounded (LeakSanitizer / leaks).
  char buf[64];
  for (int i = 0; i < 64; i++) {
    int n = snprintf(buf, sizeof buf, "A :: %d\nB :: %d\n", i, i * 2);
    db_set_source_text(&db, sid, buf, (size_t)n);
    db_request_begin(&db, db_current_revision(&db));
    db_query_file_ast(&db, fid);
    db_request_end(&db);
  }
  if (fslot(&db, fid)->computed_rev <= c1) {
    fprintf(stderr, "FAIL: edit loop did not keep reparsing\n");
    ok = 0;
  }

  db_free(&db);

  if (ok) {
    printf("PASS source_edit: identical=no-op, real=reparse+version++, "
           "64-edit loop bounded\n");
    return 0;
  }
  printf("FAIL source_edit\n");
  return 1;
}
