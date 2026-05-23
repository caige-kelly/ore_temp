// Throwaway: parse-ONLY benchmark. Times db_query_file_ast (lex +
// layout + parse + fingerprint) with NO AST dump / diag-collect /
// teardown noise. Fresh db per iteration so the query never memoizes.
// Same isolated metric as the historical ~95 MB/s figure. Not in build.
//
// Build (standalone, like the other tools/*_test.c):
//   clang -std=c17 -O2 -Isrc $(find src/support src/db src/lexer \
//     src/parser src/consumers/driver -name '*.c' ! -name main.c) \
//     tools/parse_bench.c -o ore-parse-bench
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/query/ast.h"
#include "../src/db/request/request.h"

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
  int iters = argc >= 3 ? atoi(argv[2]) : 1;
  size_t len = 0;
  char *src = read_file(argv[1], &len);
  if (!src) return 1;

  double total = 0; uint32_t nodes = 0; uint64_t fp = 0;
  for (int it = 0; it < iters; it++) {
    struct db db;
    db_init(&db);
    SourceId sid = db_create_source(&db, argv[1], strlen(argv[1]), src, len);
    ModuleId mid = db_create_module(&db, STR_ID_NONE);
    FileId fid = db_create_file(&db, sid, mid);

    db_request_begin(&db, 1);
    double t0 = now_ms();
    fp = db_query_file_ast(&db, fid);
    double t1 = now_ms();
    db_request_end(&db);

    total += (t1 - t0);
    nodes = *(uint32_t *)vec_get(&db.files.node_counts, file_id_local(fid));
    db_free(&db);
  }

  double avg = total / iters;
  double mb = (double)len / (1024.0 * 1024.0);
  printf("file=%s bytes=%zu nodes=%u iters=%d\n", argv[1], len, nodes, iters);
  printf("parse-only avg: %.2f ms  (%.1f MB/s)  fp=%llu\n",
         avg, mb / (avg / 1000.0), (unsigned long long)fp);
  free(src);
  return 0;
}
