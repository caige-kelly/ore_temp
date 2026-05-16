// Throwaway: parse-ONLY benchmark. Times db_query_module_ast (lex +
// layout + parse + fingerprint) with NO AST dump / diag-collect /
// teardown noise. Fresh db per iteration so the query never memoizes.
// Not part of the build.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/db/db.h"
#include "../src/db/query/ast.h"

static char *read_file(const char *p, size_t *n) {
  FILE *f = fopen(p, "rb");
  if (!f) { perror(p); return NULL; }
  fseek(f, 0, SEEK_END); long s = ftell(f); fseek(f, 0, SEEK_SET);
  char *b = malloc((size_t)s + 1);
  *n = fread(b, 1, (size_t)s, f); fclose(f); b[*n] = 0; return b;
}

static double now_ms(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <file.ore> [iters]\n", argv[0]); return 1; }
  int iters = argc >= 3 ? atoi(argv[3 - 1]) : 1;
  size_t len = 0;
  char *src = read_file(argv[1], &len);
  if (!src) return 1;

  double total = 0; uint32_t nodes = 0; uint64_t fp = 0;
  for (int it = 0; it < iters; it++) {
    struct db db;
    db_init(&db);
    SourceId sid = db_alloc_source(&db, argv[1], strlen(argv[1]), src, len);
    FileId fid = file_id_make_physical(sid.idx);
    ModuleId mid = db_alloc_module(&db);
    *(FileId *)vec_get(&db.modules.files, mid.idx) = fid;

    db_request_begin(&db, 1);
    double t0 = now_ms();
    fp = db_query_module_ast(&db, mid);
    double t1 = now_ms();
    db_request_end(&db);

    total += (t1 - t0);
    nodes = *(uint32_t *)vec_get(&db.modules.node_counts, mid.idx);
    db_free(&db);
  }

  double avg = total / iters;
  double mb = (double)len / (1024.0 * 1024.0);
  printf("file=%s bytes=%zu nodes=%u iters=%d\n", argv[1], len, nodes, iters);
  printf("parse-only avg: %.1f ms  (%.1f MB/s)  fp=%llu\n",
         avg, mb / (avg / 1000.0), (unsigned long long)fp);
  free(src);
  return 0;
}
