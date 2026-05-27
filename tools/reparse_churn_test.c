// Tier 1 gate — reparse churn under hash-cons.
//
// Two properties hold simultaneously under heavy edit churn:
//
//   1. **Sibling-decl green-tree pointers are pointer-equal across
//      reparses when only an unrelated decl changes.** Hash-cons (the
//      NodeCache) interns structurally-identical subtrees. If A and B
//      are top-level decls and we edit only B, A's GreenNode* should
//      be byte-identical between adjacent reparses.
//   2. **Memory stays bounded across N edits.** Run the whole loop
//      under ASan/LeakSanitizer; the test driver doesn't have to
//      assert directly — if anything leaks across reparses (forgotten
//      green_node_release, orphaned arenas, growing hashmaps without
//      compaction), the post-`db_free` leak check catches it.
//
// If hash-cons breaks (e.g., a stray content-hash regression), the
// system still produces correct output but with O(N²) memory growth
// — invisible without this test.
//
// Fixture: 10 const-int decls D0..D9. We make 100 alternating edits
// to D5's value, then verify D0..D4 and D6..D9 GreenNode pointers
// are still pointer-equal to the initial parse — meaning the
// 100 reparses all reused the cached subtrees for unchanged decls.

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/query/invalidate.h"
#include "../src/db/query/query.h"
#include "../src/db/request/request.h"
#include "../src/syntax/syntax.h"

#include <stdio.h>
#include <string.h>

extern Fingerprint db_query_file_ast(struct db *s, FileId fid);

// Snapshot of the top-level decl GreenNode pointers at a given parse.
typedef struct {
  const struct GreenNode *decls[10];
  uint32_t count;
} TopLevelSnapshot;

// Walk the file's SK_SOURCE_FILE green root and capture the GreenNode
// pointer of each direct NODE child. Tokens (whitespace + SEMI) are
// ignored. Only the first `cap` decls are recorded.
static TopLevelSnapshot capture(struct db *s, FileId fid, uint32_t cap) {
  TopLevelSnapshot snap = {{0}, 0};
  uint32_t local = file_id_local(fid);
  const struct GreenNode *root =
      *(const struct GreenNode **)vec_get(&s->files.green_roots, local);
  if (!root) return snap;
  uint32_t n = green_node_num_children(root);
  for (uint32_t i = 0; i < n && snap.count < cap; i++) {
    GreenElement c = green_node_child(root, i);
    if (c.kind == GREEN_ELEM_NODE && c.node) {
      snap.decls[snap.count++] = c.node;
    }
  }
  return snap;
}

// Assert every entry in `b` matches `a` EXCEPT the one at `changed_idx`
// (which may match or differ — we don't check it). Returns 1 on pass.
static int assert_siblings_equal(const TopLevelSnapshot *a,
                                  const TopLevelSnapshot *b,
                                  uint32_t changed_idx, const char *label) {
  if (a->count != b->count) {
    fprintf(stderr, "FAIL [%s]: snapshot count differs (a=%u, b=%u)\n",
            label, a->count, b->count);
    return 0;
  }
  int ok = 1;
  for (uint32_t i = 0; i < a->count; i++) {
    if (i == changed_idx) continue;
    if (a->decls[i] != b->decls[i]) {
      fprintf(stderr,
              "FAIL [%s]: decl[%u] GreenNode* drifted (%p -> %p) — "
              "hash-cons miss\n",
              label, i, (void *)a->decls[i], (void *)b->decls[i]);
      ok = 0;
    }
  }
  return ok;
}

int main(void) {
  struct db db;
  db_init(&db);

  // Build a 10-decl source. Each decl is one line: "Di :: <n>\n".
  // Initial values: D0..D9 = 100, 101, 102, ..., 109.
  char src_buf[512];
  size_t src_len = 0;
  for (int i = 0; i < 10; i++) {
    int written = snprintf(src_buf + src_len, sizeof(src_buf) - src_len,
                            "D%d :: %d\n", i, 100 + i);
    if (written <= 0) {
      fprintf(stderr, "FAIL: snprintf for D%d\n", i);
      db_free(&db);
      return 1;
    }
    src_len += (size_t)written;
  }

  const char *path = "churn.ore";
  SourceId sid = db_create_source(&db, path, strlen(path), src_buf, src_len);
  NamespaceId M = db_create_namespace(&db);
  FileId fid = db_create_file(&db, sid, M);

  int ok = 1;

  // Initial parse.
  db_request_begin(&db, 1);
  db_query_file_ast(&db, fid);
  db_request_end(&db);
  TopLevelSnapshot snap0 = capture(&db, fid, 10);
  if (snap0.count != 10) {
    fprintf(stderr, "FAIL: initial parse produced %u top-level decls, want 10\n",
            snap0.count);
    db_free(&db);
    printf("FAIL reparse_churn\n");
    return 1;
  }

  // ---- Property 1: edit ONLY D5's value, assert siblings hash-cons-stable. ----
  char edit_buf[512];
  size_t edit_len = 0;
  for (int i = 0; i < 10; i++) {
    int written =
        snprintf(edit_buf + edit_len, sizeof(edit_buf) - edit_len,
                 "D%d :: %d\n", i, (i == 5) ? 999 : 100 + i);
    edit_len += (size_t)written;
  }
  if (!db_set_source_text(&db, sid, edit_buf, edit_len)) {
    fprintf(stderr, "FAIL: D5 edit reported no change\n");
    ok = 0;
  }
  db_request_begin(&db, db_current_revision(&db));
  db_query_file_ast(&db, fid);
  db_request_end(&db);
  TopLevelSnapshot snap1 = capture(&db, fid, 10);
  ok &= assert_siblings_equal(&snap0, &snap1, /*changed_idx=*/5,
                                "D5-only edit");
  if (snap0.decls[5] == snap1.decls[5]) {
    fprintf(stderr, "FAIL: D5 GreenNode* unchanged across a value edit — "
                    "hash-cons false-share?\n");
    ok = 0;
  }

  // ---- Property 2: revert D5 restores the ORIGINAL D5 pointer. ----
  // Hash-cons should re-intern the original (D5 :: 105) subtree to the
  // same pointer that snap0 saw.
  if (!db_set_source_text(&db, sid, src_buf, src_len)) {
    fprintf(stderr, "FAIL: revert edit reported no change\n");
    ok = 0;
  }
  db_request_begin(&db, db_current_revision(&db));
  db_query_file_ast(&db, fid);
  db_request_end(&db);
  TopLevelSnapshot snap2 = capture(&db, fid, 10);
  for (int i = 0; i < 10; i++) {
    if (snap0.decls[i] != snap2.decls[i]) {
      fprintf(stderr,
              "FAIL: revert-to-initial decl[%d] GreenNode* drifted (%p -> %p) "
              "— NodeCache didn't re-intern the original subtree\n",
              i, (void *)snap0.decls[i], (void *)snap2.decls[i]);
      ok = 0;
    }
  }

  // ---- Property 3: 100 alternating edits — memory stays bounded. ----
  // The actual leak check is via LSan when db_free runs at the end.
  // What we verify here is that the loop COMPLETES and that, between
  // every pair of consecutive whitespace-only edits, ALL decls
  // pointer-equal to the previous parse.
  const TopLevelSnapshot *prev = &snap2;
  TopLevelSnapshot iter_snaps[2];
  int slot = 0;
  for (int it = 0; it < 100; it++) {
    // Even iterations: append a trailing newline (whitespace-only edit).
    // Odd iterations: revert to canonical (drops the trailing newline).
    edit_len = src_len;
    memcpy(edit_buf, src_buf, src_len);
    if (it % 2 == 0) {
      edit_buf[edit_len++] = '\n';
    }
    if (!db_set_source_text(&db, sid, edit_buf, edit_len) && it % 2 != 0) {
      // Reverting a parse that hadn't changed produces no-op. That's
      // expected on odd iterations after the first cycle.
    }
    db_request_begin(&db, db_current_revision(&db));
    db_query_file_ast(&db, fid);
    db_request_end(&db);
    iter_snaps[slot] = capture(&db, fid, 10);
    // Whitespace-only edit: every decl's GreenNode* must equal the
    // initial snapshot's. (No "changed" idx — pass an out-of-range
    // sentinel.)
    if (!assert_siblings_equal(&snap0, &iter_snaps[slot], /*changed_idx=*/99,
                                 "trailing-newline churn")) {
      ok = 0;
      break;
    }
    prev = &iter_snaps[slot];
    slot ^= 1;
    (void)prev;
  }

  db_free(&db);

  if (ok) {
    printf("PASS reparse_churn: hash-cons preserves sibling GreenNode* "
           "across 100+ edits; revert re-interns to original\n");
    return 0;
  }
  printf("FAIL reparse_churn\n");
  return 1;
}
