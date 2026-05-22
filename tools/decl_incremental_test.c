// Gap A gate — per-decl typecheck cutoff.
//
// One file, two functions A and B. Build (rev 1). Edit ONLY A's body
// via the real db_set_source_text API; re-typecheck (rev 2). Assert:
//
//   - QUERY_DECL_AST(A)'s fingerprint CHANGED; QUERY_DECL_AST(B)'s is
//     UNCHANGED (the structural-hash proof — B's subtree shifted ids
//     and token indices, but the hash is position-independent).
//   - A's type_of_def / fn_signature / infer_body / body_scopes all
//     recomputed (computed_rev advanced to 2).
//   - B's four sema slots are FROZEN at rev 1 — B did not re-typecheck.
//     This is the Gap A assertion: editing A's body must not touch B.
//
// Sub-test 2: a whitespace-only edit inside A's body reparses the file
// but reproduces QUERY_DECL_AST(A)'s fingerprint (trivia isn't tokens)
// → even A's sema slots stay frozen — nothing re-typechecks.
//
// Sub-test 3: renaming B's parameter changes QUERY_DECL_AST(B)'s
// fingerprint (it folds the param's name StrId, a non-kind `extra`
// slot) → B recomputes. Catches an omitted slot in ast_hash_node.

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/query/ast.h"
#include "../src/db/query/def_identity.h"
#include "../src/db/query/invalidate.h"
#include "../src/db/query/query.h"
#include "../src/db/request/request.h"
#include "../src/sema/sema.h"

#include <stdio.h>
#include <string.h>

extern Fingerprint db_query_top_level_index(struct db *s, ModuleId mod);

// computed_rev of a DefId-keyed sema slot (COLD column).
static uint64_t sema_rev(struct db *s, QueryKind kind, DefId def) {
  QuerySlotCold *sl = db_locate_slot_cold(s, kind, (uint64_t)def.idx);
  return sl ? sl->computed_rev : 0;
}

// QUERY_DECL_AST slot key + fingerprint accessor.
static uint64_t decl_ast_key(FileId fid, AstId ast_id) {
  return ((uint64_t)file_id_local(fid) << 32) | (uint64_t)ast_id.idx;
}
static Fingerprint decl_ast_fp(struct db *s, FileId fid, AstId ast_id) {
  QuerySlotHot *sl = db_locate_slot(s, QUERY_DECL_AST, decl_ast_key(fid, ast_id));
  return sl ? sl->fingerprint : FINGERPRINT_NONE;
}

// The four DefId-keyed sema queries driven by sema_check_module.
static const QueryKind SEMA_KINDS[4] = {
    QUERY_TYPE_OF_DECL, QUERY_FN_SIGNATURE, QUERY_INFER_BODY, QUERY_BODY_SCOPES};

int main(void) {
  struct db db;
  db_init(&db);

  const char *path = "m.ore";
  // Two functions; A's body is on line 2, B's on line 4. The edits
  // below only ever touch A's body line or B's signature line.
  const char *t1 = "A :: fn(x: f32) f32\n"
                   "    x * x\n"
                   "B :: fn(y: f32) f32\n"
                   "    y + y\n";
  // Edit 1: A's body only ( * -> - ). B's two lines are byte-identical.
  const char *t2 = "A :: fn(x: f32) f32\n"
                   "    x - x\n"
                   "B :: fn(y: f32) f32\n"
                   "    y + y\n";
  // Edit 2: intra-line whitespace inside A's body (not indentation).
  const char *t3 = "A :: fn(x: f32) f32\n"
                   "    x -  x\n"
                   "B :: fn(y: f32) f32\n"
                   "    y + y\n";
  // Edit 3: rename B's parameter y -> z (a non-kind `extra` slot).
  const char *t4 = "A :: fn(x: f32) f32\n"
                   "    x -  x\n"
                   "B :: fn(z: f32) f32\n"
                   "    z + z\n";

  SourceId src = db_create_source(&db, path, strlen(path), t1, strlen(t1));
  ModuleId M = db_create_module(&db);
  FileId fid = db_create_file(&db, src, M);
  db_add_file_to_module(&db, M, fid);

  int ok = 1;

  // ---- Request 1 (rev 1): parse + typecheck. ----
  db_request_begin(&db, 1);
  (void)db_query_file_ast(&db, fid);

  // Top-level index gives each decl's stable AstId (entry 0 = A, 1 = B).
  FileArray *tli =
      (FileArray *)vec_get(&db.files.top_level_indices, file_id_local(fid));
  if (!tli || tli->count < 2) {
    fprintf(stderr, "FAIL: expected 2 top-level decls, got %u\n",
            tli ? tli->count : 0);
    db_request_end(&db);
    db_free(&db);
    printf("FAIL decl_incremental\n");
    return 1;
  }
  AstId astA = ((TopLevelEntry *)tli->data)[0].ast_id;
  AstId astB = ((TopLevelEntry *)tli->data)[1].ast_id;

  sema_check_module(&db, M);
  DefId defA = db_query_def_identity(&db, M, astA);
  DefId defB = db_query_def_identity(&db, M, astB);
  db_request_end(&db);

  Fingerprint declA_fp1 = decl_ast_fp(&db, fid, astA);
  Fingerprint declB_fp1 = decl_ast_fp(&db, fid, astB);
  for (int i = 0; i < 4; i++) {
    if (sema_rev(&db, SEMA_KINDS[i], defA) != 1 ||
        sema_rev(&db, SEMA_KINDS[i], defB) != 1) {
      fprintf(stderr, "FAIL: rev1 sema slot not at computed_rev 1\n");
      ok = 0;
    }
  }

  // ---- Edit 1: A's body only. Re-typecheck (rev 2). ----
  if (!db_set_source_text(&db, src, t2, strlen(t2))) {
    fprintf(stderr, "FAIL: edit 1 reported no change\n");
    ok = 0;
  }
  uint64_t rev2 = db_current_revision(&db);
  db_request_begin(&db, rev2);
  sema_check_module(&db, M);
  db_request_end(&db);

  if (decl_ast_fp(&db, fid, astA) == declA_fp1) {
    fprintf(stderr, "FAIL: QUERY_DECL_AST(A) fingerprint unchanged after "
                    "an A-body edit\n");
    ok = 0;
  }
  if (decl_ast_fp(&db, fid, astB) != declB_fp1) {
    fprintf(stderr, "FAIL: QUERY_DECL_AST(B) fingerprint changed though B "
                    "was not edited — structural hash is position-dependent\n");
    ok = 0;
  }
  for (int i = 0; i < 4; i++) {
    if (sema_rev(&db, SEMA_KINDS[i], defA) != rev2) {
      fprintf(stderr, "FAIL: A sema slot %d not recomputed (rev %llu, want %llu)\n",
              i, (unsigned long long)sema_rev(&db, SEMA_KINDS[i], defA),
              (unsigned long long)rev2);
      ok = 0;
    }
    if (sema_rev(&db, SEMA_KINDS[i], defB) != 1) {
      fprintf(stderr,
              "FAIL: B sema slot %d RE-TYPECHECKED on an A-only edit "
              "(computed_rev %llu, want 1) — Gap A not closed\n",
              i, (unsigned long long)sema_rev(&db, SEMA_KINDS[i], defB));
      ok = 0;
    }
  }

  // ---- Edit 2: whitespace-only inside A's body. ----
  if (!db_set_source_text(&db, src, t3, strlen(t3))) {
    fprintf(stderr, "FAIL: edit 2 (whitespace) reported no change\n");
    ok = 0;
  }
  db_request_begin(&db, db_current_revision(&db));
  sema_check_module(&db, M);
  db_request_end(&db);

  for (int i = 0; i < 4; i++) {
    if (sema_rev(&db, SEMA_KINDS[i], defA) != rev2 ||
        sema_rev(&db, SEMA_KINDS[i], defB) != 1) {
      fprintf(stderr, "FAIL: whitespace-only edit re-typechecked a decl "
                      "(slot %d) — structural fp not trivia-insensitive\n",
              i);
      ok = 0;
    }
  }

  // ---- Edit 3: rename B's parameter. B must recompute. ----
  Fingerprint declB_fp3 = decl_ast_fp(&db, fid, astB);
  if (!db_set_source_text(&db, src, t4, strlen(t4))) {
    fprintf(stderr, "FAIL: edit 3 (param rename) reported no change\n");
    ok = 0;
  }
  uint64_t rev4 = db_current_revision(&db);
  db_request_begin(&db, rev4);
  sema_check_module(&db, M);
  db_request_end(&db);

  if (decl_ast_fp(&db, fid, astB) == declB_fp3) {
    fprintf(stderr, "FAIL: QUERY_DECL_AST(B) fingerprint unchanged after a "
                    "param rename — ast_hash_node omits the name slot\n");
    ok = 0;
  }
  if (sema_rev(&db, SEMA_KINDS[0], defB) != rev4) {
    fprintf(stderr, "FAIL: B did not recompute after its param was renamed\n");
    ok = 0;
  }

  db_free(&db);
  if (ok) {
    printf("PASS decl_incremental: edit A's body -> only A re-typechecks "
           "(B frozen); whitespace = no-op; param rename re-checks B\n");
    return 0;
  }
  printf("FAIL decl_incremental\n");
  return 1;
}
