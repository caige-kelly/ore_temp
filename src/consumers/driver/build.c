#include "../../db/storage/vec.h"
#include "../../db/diag/diag.h"
#include "../../db/db.h"
#include "../../db/query/ast.h"
#include "options.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  if (!src) return EXIT_FAILURE;

  struct db db;
  db_init(&db);

  SourceId sid = db_alloc_source(&db, opts->input_path, strlen(opts->input_path), src, src_len);
  FileId fid = file_id_make_physical(sid.idx);

  ModuleId mid = db_alloc_module(&db);
  *(FileId*)vec_get(&db.modules.files, mid.idx) = fid;

  db_request_begin(&db, 1);
  Fingerprint fp = db_query_module_ast(&db, mid);
  db_request_end(&db);

  printf("Successfully parsed module! AST Fingerprint: %llu\n", (unsigned long long)fp);

  db_free(&db);
  free(src);
  return EXIT_SUCCESS;
}
