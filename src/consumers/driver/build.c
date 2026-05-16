#include "../../db/db.h"
#include "../../db/diag/diag.h"
#include "../../db/query/ast.h"
#include "../../db/storage/vec.h"
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

  SourceId sid = db_alloc_source(&db, opts->input_path,
                                 strlen(opts->input_path), src, src_len);
  FileId fid = file_id_make_physical(sid.idx);

  ModuleId mid = db_alloc_module(&db);
  *(FileId *)vec_get(&db.modules.files, mid.idx) = fid;

  db_request_begin(&db, 1);
  Fingerprint fp = db_query_module_ast(&db, mid);
  db_request_end(&db);

  // Honest oracle: success is "zero error diagnostics", not "the query
  // returned". Collect this file's slot-keyed diagnostics and render.
  Vec diags;
  vec_init(&diags, sizeof(Diag));
  db_diag_collect_for_file(&db, fid, &diags);

  size_t errors = 0;
  for (size_t i = 0; i < diags.count; i++) {
    Diag *d = (Diag *)vec_get(&diags, i);
    char buf[1024];
    db_diag_format(&db, d, buf, sizeof buf);
    fprintf(stderr, "%s: %s\n",
            d->severity == DIAG_ERROR     ? "error"
            : d->severity == DIAG_WARNING ? "warning"
                                          : "note",
            buf);
    if (d->severity == DIAG_ERROR)
      errors++;
  }

  if (errors == 0)
    printf("OK: %s parsed (fingerprint %llu)\n", opts->input_path,
           (unsigned long long)fp);
  else
    printf("FAIL: %s — %zu parse error(s)\n", opts->input_path, errors);

  ASTStore *ast = *(ASTStore **)vec_get(&db.modules.asts, mid.idx);
  Vec *top_level_index = (Vec *)vec_get(&db.modules.top_level_indices, mid.idx);
  ast_dump_module(ast, top_level_index, &db.strings);

  vec_free(&diags);
  db_free(&db);
  free(src);
  return errors == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
