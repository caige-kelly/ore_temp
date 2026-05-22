#include "../../db/db.h"
#include "../../db/diag/diag.h"
#include "../../db/query/ast.h"
#include "../../db/query/invalidate.h"
#include "../../db/query/query.h"
#include "../../db/storage/vec.h"
#include "../../sema/sema.h"
#include "options.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast_dump_inc.h"

static char *slurp_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "could not open %s\n", path);
    return NULL;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long sz = ftell(f);
  if (sz < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  char *buf = (char *)malloc((size_t)sz + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  size_t n = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  if (n != (size_t)sz) {
    free(buf);
    return NULL;
  }
  buf[sz] = '\0';
  if (out_len)
    *out_len = (size_t)sz;
  return buf;
}

int driver_build_run(const struct CompilerOptions *opts) {
  size_t src_len = 0;
  char *src = slurp_file(opts->input_path, &src_len);
  if (!src)
    return EXIT_FAILURE;

  struct db db;
  db_init(&db);

  SourceId sid = db_create_source(&db, opts->input_path,
                                  strlen(opts->input_path), src, src_len);

  // source -> file -> module: distinct id spaces. 1:1 today (one file
  // per module); db_create_file stamps the file's source/module
  // back-refs and the module records this file in its file list.
  ModuleId mid = db_create_module(&db);
  FileId fid = db_create_file(&db, sid, mid);
  db_add_file_to_module(&db, mid, fid);

  // ORE_PROFILE_LOOP=N — re-run the parse query N times for sampling
  // (each iteration after the first stales the slot so the work is
  // actually redone). Temporary; not for production use.
  int loops = 1;
  const char *lp = getenv("ORE_PROFILE_LOOP");
  if (lp) {
    int n = atoi(lp);
    if (n > 1)
      loops = n;
  }

  Fingerprint fp = 0;
  for (int li = 0; li < loops; li++) {
    if (li > 0) {
      QuerySlot *sl = db_locate_slot(
          &db, QUERY_FILE_AST, vec_get(&db.files.ids, file_id_local(fid)));
      if (sl) {
        sl->state = QUERY_EMPTY;
        sl->fingerprint = FINGERPRINT_NONE;
      }
    }
    db_request_begin(&db, (uint32_t)(li + 1));
    fp = db_query_file_ast(&db, fid);
    db_request_end(&db);
  }

  // Hand off to sema: build scopes, materialize DefIds, type every
  // top-level decl, infer fn bodies. All work cached behind salsa slots.
  // Sema is what emits type-error diagnostics, so it MUST run before
  // we collect. One request wraps sema + diag-collect + dump so the
  // effective revision is pinned at current_rev across the whole
  // verification pass (sema is a query CONSUMER and does not open
  // requests itself).
  db_request_begin(&db, db_current_revision(&db));
  sema_check_module(&db, mid);

  // Collect parse + sema diagnostics. Per-slot ownership means cached
  // queries replay their diags on subsequent calls without re-running
  // the body — that's the salsa early-cutoff win for IDE/LSP use.
  Vec diags;
  vec_init(&diags, sizeof(Diag));
  db_collect_diags_for_file(&db, fid, &diags);

  size_t errors = 0;
  for (size_t i = 0; i < diags.count; i++) {
    Diag *d = (Diag *)vec_get(&diags, i);
    db_print_diag(&db, d, stderr);
    if (d->severity == DIAG_ERROR)
      errors++;
  }

  if (errors == 0)
    printf("OK: %s\n", opts->input_path);
  else
    printf("FAIL: %s — %zu error(s)\n", opts->input_path, errors);

  if (!getenv("ORE_NO_DUMP"))
    sema_dump_module(&db, mid);

  uint32_t f = file_id_local(fid);
  ASTStore *ast = *(ASTStore **)vec_get(&db.files.asts, f);
  Vec *top_level_index = (Vec *)vec_get(&db.files.top_level_indices, f);
  if (!getenv("ORE_NO_DUMP"))
    ast_dump_module(ast, top_level_index, &db.strings);

  db_request_end(&db);

  vec_free(&diags);
  db_free(&db);
  free(src);
  return errors == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
