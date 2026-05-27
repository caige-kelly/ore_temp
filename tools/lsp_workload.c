// LSP profile workload — standardized scenarios that exercise the
// compiler's incremental machinery on a REAL file (default:
// examples/allocator/allocator.ore) and emit per-iteration metrics
// (RSS memory + cumulative + delta query-cache counts + table sizes
// + wall time).
//
// USAGE
//   ./ore-lsp-workload <scenario> [--file PATH] [--iters N] [--csv]
//                                  [--attach-pause]
//
// SCENARIOS
//   steady-typecheck  open file once; re-typecheck N times
//   edit-replace      alternate two contents N times
//   noop-edit         apply the SAME text N times (hash fast-path)
//   evict-churn       open/evict cycles N times
//   cross-file        a.ore @imports the real file; edit imported N times
//   lazy-load         one importer with N @imports of the real file
//   all               run every scenario sequentially
//
// FLAGS
//   --file PATH        source of "real" content; default
//                      examples/allocator/allocator.ore
//   --iters N          number of iterations (default 100)
//   --csv              machine-parseable CSV (header + rows)
//   --attach-pause     print PID + wait for Enter (for Instruments)
//
// PER-ITER COLUMNS (CSV header):
//   iter, scenario, rss_kb,
//   compute_total, compute_delta,
//   cached_total,  cached_delta,
//   sources_count, intern_count, wall_us

#define _POSIX_C_SOURCE 199309L
#include "../src/db/db.h"
#include "../src/db/intern_pool/intern_pool.h"
#include "../src/db/query/query.h"
#include "../src/db/request/request.h"
#include "../src/db/workspace/workspace.h"
#include "../src/sema/sema.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// === Generic helpers ====================================================

static uint64_t now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

// Peak RSS in KB. getrusage's ru_maxrss is bytes on macOS, KB on
// Linux. Peak grows monotonically with workload — exactly what we
// want for spotting leaks.
static uint64_t rss_kb(void) {
  struct rusage ru;
  if (getrusage(RUSAGE_SELF, &ru) != 0)
    return 0;
#if defined(__APPLE__)
  return (uint64_t)ru.ru_maxrss / 1024ULL;
#else
  return (uint64_t)ru.ru_maxrss;
#endif
}

static char *read_file_or_die(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "lsp-workload: cannot open %s: %s\n", path,
            strerror(errno));
    exit(1);
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  rewind(f);
  char *buf = (char *)malloc((size_t)sz + 1);
  if (!buf) {
    fclose(f);
    fprintf(stderr, "lsp-workload: malloc failed for %s\n", path);
    exit(1);
  }
  if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
    fclose(f);
    free(buf);
    fprintf(stderr, "lsp-workload: read failed for %s\n", path);
    exit(1);
  }
  buf[sz] = '\0';
  fclose(f);
  if (out_len)
    *out_len = (size_t)sz;
  return buf;
}

// V1 = base content as-is; V2 = base content + an appended marker.
// Same shape as real "user typed a comment line" edits — small,
// realistic, non-trivial fingerprint shift.
static char *append_marker(const char *base, size_t base_len,
                            const char *marker, size_t *out_len) {
  size_t marker_len = strlen(marker);
  char *buf = (char *)malloc(base_len + marker_len + 1);
  memcpy(buf, base, base_len);
  memcpy(buf + base_len, marker, marker_len);
  buf[base_len + marker_len] = '\0';
  if (out_len)
    *out_len = base_len + marker_len;
  return buf;
}

static void write_file_or_die(const char *path, const char *content,
                              size_t len) {
  int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
  if (fd < 0) {
    perror("open");
    exit(1);
  }
  if (write(fd, content, len) != (ssize_t)len) {
    perror("write");
    exit(1);
  }
  close(fd);
}

static void unlink_safe(const char *path) { (void)unlink(path); }

// === Per-iteration sample (with deltas) =================================

typedef struct {
  uint32_t    iter;
  const char *scenario;
  uint64_t    rss_kb;
  uint64_t    compute_total;
  uint64_t    cached_total;
  uint64_t    compute_delta;
  uint64_t    cached_delta;
  uint64_t    sources_count;
  uint64_t    intern_count;
  uint64_t    wall_us;
  // Pool sizes — for spotting compaction-driven oscillation patterns.
  // (node_types_pool / node_to_scope removed in green-tree refactor —
  //  per-decl HashMaps replaced shared pools.)
  uint64_t    body_scope_rows;
  uint64_t    body_scope_binds;
  uint64_t    decl_pool;
  // Compaction events — n_compactions sums across the 2 pool families
  // (body_scope rows+binds compact together; decl_pool is independent).
  uint64_t    n_compactions_total;
  uint64_t    compact_ns_delta; // ns spent in compaction THIS iter
} Sample;

static void aggregate_stats(struct db *s, uint64_t *compute,
                            uint64_t *cached) {
  *compute = 0;
  *cached = 0;
  for (int k = 0; k < QUERY_KIND_COUNT; k++) {
    *compute += s->query_stats[k].compute;
    *cached += s->query_stats[k].cached_hit;
  }
}

// Sum the 2-element compact_stats array fields. Index 0 = body_scope
// (rows + binds compact together), 1 = decl_pool.
static uint64_t sum2(const uint64_t arr[2]) {
  return arr[0] + arr[1];
}

static void capture(struct db *s, Sample *out, uint32_t iter,
                    const char *scenario, uint64_t start_us,
                    uint64_t prev_compute, uint64_t prev_cached,
                    uint64_t prev_compact_ns) {
  out->iter = iter;
  out->scenario = scenario;
  out->rss_kb = rss_kb();
  aggregate_stats(s, &out->compute_total, &out->cached_total);
  out->compute_delta = out->compute_total - prev_compute;
  out->cached_delta = out->cached_total - prev_cached;
  out->sources_count = s->sources.hashes.count;
  out->intern_count = s->intern.items_count;
  out->wall_us = now_us() - start_us;

  // Pool sizes (post-iter, so AFTER any compaction at db_request_end).
  out->body_scope_rows  = s->body_scope_rows.count;
  out->body_scope_binds = s->body_scope_binds.count;
  out->decl_pool        = s->scopes.decl_pool.count;

  // Compaction events — total since db_init.
  out->n_compactions_total = sum2(s->compact_stats.n_compactions);
  uint64_t cur_ns = sum2(s->compact_stats.total_ns);
  out->compact_ns_delta = cur_ns - prev_compact_ns;
}

static void print_csv_header(void) {
  printf("iter,scenario,rss_kb,"
         "compute_total,compute_delta,"
         "cached_total,cached_delta,"
         "sources_count,intern_count,wall_us,"
         "body_scope_rows,body_scope_binds,decl_pool,"
         "n_compactions,compact_ns_delta\n");
}

static void print_sample(const Sample *s, bool csv) {
  if (csv) {
    printf("%u,%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
           ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
           ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
           ",%" PRIu64 ",%" PRIu64 "\n",
           s->iter, s->scenario, s->rss_kb, s->compute_total,
           s->compute_delta, s->cached_total, s->cached_delta,
           s->sources_count, s->intern_count, s->wall_us,
           s->body_scope_rows, s->body_scope_binds, s->decl_pool,
           s->n_compactions_total, s->compact_ns_delta);
  } else {
    fprintf(stderr,
            "[%4u %-18s] rss=%6" PRIu64 "k  Ccomp=%6" PRIu64
            " (Δ%5" PRIu64 ")  Ccache=%6" PRIu64 " (Δ%5" PRIu64
            ")  srcs=%3" PRIu64 "  intern=%6" PRIu64 "  +%" PRIu64
            "us  pools=[bsr=%" PRIu64 " bsb=%" PRIu64
            " dp=%" PRIu64 "]  comp=%" PRIu64
            " (+%" PRIu64 "ns)\n",
            s->iter, s->scenario, s->rss_kb, s->compute_total,
            s->compute_delta, s->cached_total, s->cached_delta,
            s->sources_count, s->intern_count, s->wall_us,
            s->body_scope_rows, s->body_scope_binds, s->decl_pool,
            s->n_compactions_total, s->compact_ns_delta);
  }
}

// === Scenarios ==========================================================

static void scenario_steady_typecheck(int iters, bool csv,
                                       const char *file_path) {
  struct db db;
  db_init(&db);

  size_t v1_len = 0;
  char *v1 = read_file_or_die(file_path, &v1_len);

  const char *tmp = "/tmp/lsp_workload_steady.ore";
  write_file_or_die(tmp, v1, v1_len);

  SourceId src = workspace_did_open(&db, tmp, strlen(tmp), v1, v1_len);
  FileId fid = db_lookup_file_by_source(&db, src);
  NamespaceId nsid = db_get_file_namespace(&db, fid);

  uint64_t prev_c = 0, prev_h = 0;
  uint64_t prev_ns = 0;
  for (int i = 0; i <= iters; i++) {
    uint64_t t0 = now_us();
    db_request_begin(&db, db_current_revision(&db));
    sema_check_module(&db, nsid);
    db_request_end(&db);
    Sample smp;
    capture(&db, &smp, (uint32_t)i, "steady-typecheck", t0, prev_c, prev_h, prev_ns);
    print_sample(&smp, csv);
    prev_c = smp.compute_total;
    prev_h = smp.cached_total;
    prev_ns += smp.compact_ns_delta;
  }

#ifdef ORE_DEBUG_QUERIES
  fprintf(stderr, "\n== steady-typecheck query stats ==\n");
  db_dump_query_stats(&db, stderr);
#endif

  db_free(&db);
  free(v1);
  unlink_safe(tmp);
}

static void scenario_edit_replace(int iters, bool csv,
                                   const char *file_path) {
  struct db db;
  db_init(&db);

  size_t v1_len = 0;
  char *v1 = read_file_or_die(file_path, &v1_len);
  size_t v2_len = 0;
  // Real decl, not just a comment — comments are lexer trivia and
  // produce no AST nodes, so V1↔V2 alternation via comment-only edits
  // is invisible to downstream sema. A real decl forces decl_ast +
  // top_level_index fingerprints to shift.
  char *v2 = append_marker(v1, v1_len,
                            "\nLSP_WORKLOAD_EDIT_MARKER :: 0\n", &v2_len);

  const char *tmp = "/tmp/lsp_workload_edit.ore";
  write_file_or_die(tmp, v1, v1_len);

  SourceId src = workspace_did_open(&db, tmp, strlen(tmp), v1, v1_len);
  FileId fid = db_lookup_file_by_source(&db, src);
  NamespaceId nsid = db_get_file_namespace(&db, fid);

  uint64_t prev_c = 0, prev_h = 0;
  uint64_t prev_ns = 0;
  {
    // iter 0: cold typecheck of V1
    uint64_t t0 = now_us();
    db_request_begin(&db, db_current_revision(&db));
    sema_check_module(&db, nsid);
    db_request_end(&db);
    Sample smp0;
    capture(&db, &smp0, 0, "edit-replace", t0, prev_c, prev_h, prev_ns);
    print_sample(&smp0, csv);
    prev_c = smp0.compute_total;
    prev_h = smp0.cached_total;
    prev_ns += smp0.compact_ns_delta;
  }

  for (int i = 1; i <= iters; i++) {
    uint64_t s_us = now_us();
    const char *t = (i % 2 == 1) ? v2 : v1;
    size_t tl = (i % 2 == 1) ? v2_len : v1_len;
    workspace_did_change(&db, tmp, strlen(tmp), t, tl);
    db_request_begin(&db, db_current_revision(&db));
    sema_check_module(&db, nsid);
    db_request_end(&db);
    Sample smp;
    capture(&db, &smp, (uint32_t)i, "edit-replace", s_us, prev_c, prev_h, prev_ns);
    print_sample(&smp, csv);
    prev_c = smp.compute_total;
    prev_h = smp.cached_total;
    prev_ns += smp.compact_ns_delta;
  }

#ifdef ORE_DEBUG_QUERIES
  fprintf(stderr, "\n== edit-replace query stats ==\n");
  db_dump_query_stats(&db, stderr);
#endif

  db_free(&db);
  free(v1);
  free(v2);
  unlink_safe(tmp);
}

// noop-edit — applies the SAME text repeatedly. Exercises the
// db_set_source_text hash fast-path ([setters/source.c:88-95]):
// when new_hash == old_hash + new_len == old_len, the call returns
// immediately without bumping revision or staling any memo.
// Per-iter compute should be ZERO after iter 0.
static void scenario_noop_edit(int iters, bool csv,
                                const char *file_path) {
  struct db db;
  db_init(&db);

  size_t v1_len = 0;
  char *v1 = read_file_or_die(file_path, &v1_len);

  const char *tmp = "/tmp/lsp_workload_noop.ore";
  write_file_or_die(tmp, v1, v1_len);

  SourceId src = workspace_did_open(&db, tmp, strlen(tmp), v1, v1_len);
  FileId fid = db_lookup_file_by_source(&db, src);
  NamespaceId nsid = db_get_file_namespace(&db, fid);

  uint64_t prev_c = 0, prev_h = 0;
  uint64_t prev_ns = 0;
  {
    // iter 0: cold typecheck
    uint64_t t0 = now_us();
    db_request_begin(&db, db_current_revision(&db));
    sema_check_module(&db, nsid);
    db_request_end(&db);
    Sample smp0;
    capture(&db, &smp0, 0, "noop-edit", t0, prev_c, prev_h, prev_ns);
    print_sample(&smp0, csv);
    prev_c = smp0.compute_total;
    prev_h = smp0.cached_total;
    prev_ns += smp0.compact_ns_delta;
  }

  for (int i = 1; i <= iters; i++) {
    uint64_t s_us = now_us();
    // Re-apply IDENTICAL text. The hash fast-path should detect this
    // and no-op (no revision bump, no slot staling).
    workspace_did_change(&db, tmp, strlen(tmp), v1, v1_len);
    db_request_begin(&db, db_current_revision(&db));
    sema_check_module(&db, nsid);
    db_request_end(&db);
    Sample smp;
    capture(&db, &smp, (uint32_t)i, "noop-edit", s_us, prev_c, prev_h, prev_ns);
    print_sample(&smp, csv);
    prev_c = smp.compute_total;
    prev_h = smp.cached_total;
    prev_ns += smp.compact_ns_delta;
  }

#ifdef ORE_DEBUG_QUERIES
  fprintf(stderr, "\n== noop-edit query stats ==\n");
  db_dump_query_stats(&db, stderr);
#endif

  db_free(&db);
  free(v1);
  unlink_safe(tmp);
}

static void scenario_evict_churn(int iters, bool csv,
                                  const char *file_path) {
  struct db db;
  db_init(&db);

  size_t v1_len = 0;
  char *v1 = read_file_or_die(file_path, &v1_len);

  uint64_t prev_c = 0, prev_h = 0;
  uint64_t prev_ns = 0;
  for (int i = 1; i <= iters; i++) {
    uint64_t s_us = now_us();
    char path[64];
    snprintf(path, sizeof path, "/tmp/lsp_workload_evict_%d.ore", i);
    write_file_or_die(path, v1, v1_len);

    SourceId src = workspace_did_open(&db, path, strlen(path), v1, v1_len);
    FileId fid = db_lookup_file_by_source(&db, src);
    NamespaceId nsid = db_get_file_namespace(&db, fid);
    db_request_begin(&db, db_current_revision(&db));
    sema_check_module(&db, nsid);
    db_request_end(&db);

    workspace_did_evict_source(&db, path, strlen(path));
    unlink_safe(path);

    Sample smp;
    capture(&db, &smp, (uint32_t)i, "evict-churn", s_us, prev_c, prev_h, prev_ns);
    if ((i % 10) == 0 || i == iters)
      print_sample(&smp, csv);
    prev_c = smp.compute_total;
    prev_h = smp.cached_total;
    prev_ns += smp.compact_ns_delta;
  }

#ifdef ORE_DEBUG_QUERIES
  fprintf(stderr, "\n== evict-churn query stats ==\n");
  db_dump_query_stats(&db, stderr);
#endif

  db_free(&db);
  free(v1);
}

static void scenario_cross_file(int iters, bool csv,
                                 const char *file_path) {
  struct db db;
  db_init(&db);

  size_t imp_len = 0;
  char *imp_v1 = read_file_or_die(file_path, &imp_len);
  size_t imp_v2_len = 0;
  // Real decl in V2 so the imported file's namespace structurally
  // changes (new pub field appears). Tests that a's queries depending
  // on b stay cached while b's queries re-run.
  char *imp_v2 = append_marker(imp_v1, imp_len,
                                "\nLSP_WORKLOAD_CROSS_MARKER :: pub 0\n",
                                &imp_v2_len);

  const char *path_b = "/tmp/lsp_workload_b.ore";
  write_file_or_die(path_b, imp_v1, imp_len);

  // Importer is synthetic — it pulls in the real file and references
  // a few of its public decls. The actual typecheck cost of resolving
  // those is what we're measuring per-iter.
  const char *a_content =
      "b :: @import(\"/tmp/lsp_workload_b.ore\")\n"
      "use_pi :: pub b.PAGE_SIZE\n";
  const char *path_a = "/tmp/lsp_workload_a.ore";
  write_file_or_die(path_a, a_content, strlen(a_content));

  SourceId src_a = workspace_did_open(&db, path_a, strlen(path_a),
                                      a_content, strlen(a_content));
  FileId fid_a = db_lookup_file_by_source(&db, src_a);
  NamespaceId nsid_a = db_get_file_namespace(&db, fid_a);

  uint64_t prev_c = 0, prev_h = 0;
  uint64_t prev_ns = 0;
  {
    // iter 0: lazy-loads b.ore
    uint64_t t0 = now_us();
    db_request_begin(&db, db_current_revision(&db));
    sema_check_module(&db, nsid_a);
    db_request_end(&db);
    Sample smp0;
    capture(&db, &smp0, 0, "cross-file", t0, prev_c, prev_h, prev_ns);
    print_sample(&smp0, csv);
    prev_c = smp0.compute_total;
    prev_h = smp0.cached_total;
    prev_ns += smp0.compact_ns_delta;
  }

  for (int i = 1; i <= iters; i++) {
    uint64_t s_us = now_us();
    const char *t = (i % 2 == 1) ? imp_v2 : imp_v1;
    size_t tl = (i % 2 == 1) ? imp_v2_len : imp_len;
    workspace_did_change_external(&db, path_b, strlen(path_b), t, tl);
    db_request_begin(&db, db_current_revision(&db));
    sema_check_module(&db, nsid_a);
    db_request_end(&db);
    Sample smp;
    capture(&db, &smp, (uint32_t)i, "cross-file", s_us, prev_c, prev_h, prev_ns);
    print_sample(&smp, csv);
    prev_c = smp.compute_total;
    prev_h = smp.cached_total;
    prev_ns += smp.compact_ns_delta;
  }

#ifdef ORE_DEBUG_QUERIES
  fprintf(stderr, "\n== cross-file query stats ==\n");
  db_dump_query_stats(&db, stderr);
#endif

  db_free(&db);
  free(imp_v1);
  free(imp_v2);
  unlink_safe(path_a);
  unlink_safe(path_b);
}

static void scenario_lazy_load(int iters, bool csv,
                                const char *file_path) {
  struct db db;
  db_init(&db);

  size_t target_len = 0;
  char *target = read_file_or_die(file_path, &target_len);

  // Create `iters` copies of the real file as import targets, plus
  // one importer that @imports them all and references PAGE_SIZE
  // from each (forces the namespace_type build).
  size_t importer_cap = 64 + (size_t)iters * 120;
  char *importer_text = (char *)malloc(importer_cap);
  size_t off = 0;
  for (int i = 1; i <= iters; i++) {
    char tp[64];
    snprintf(tp, sizeof tp, "/tmp/lsp_workload_lz_%d.ore", i);
    write_file_or_die(tp, target, target_len);
    off += (size_t)snprintf(
        importer_text + off, importer_cap - off,
        "m%d :: @import(\"%s\")\nx%d :: pub m%d.PAGE_SIZE\n",
        i, tp, i, i);
  }

  const char *importer_path = "/tmp/lsp_workload_lz_main.ore";
  write_file_or_die(importer_path, importer_text, strlen(importer_text));

  uint64_t t0 = now_us();
  SourceId src = workspace_did_open(&db, importer_path, strlen(importer_path),
                                    importer_text, strlen(importer_text));
  FileId fid = db_lookup_file_by_source(&db, src);
  NamespaceId nsid = db_get_file_namespace(&db, fid);
  db_request_begin(&db, db_current_revision(&db));
  sema_check_module(&db, nsid);
  db_request_end(&db);
  Sample smp;
  capture(&db, &smp, (uint32_t)iters, "lazy-load", t0, 0, 0, 0);
  print_sample(&smp, csv);

#ifdef ORE_DEBUG_QUERIES
  fprintf(stderr, "\n== lazy-load query stats ==\n");
  db_dump_query_stats(&db, stderr);
#endif

  db_free(&db);
  unlink_safe(importer_path);
  for (int i = 1; i <= iters; i++) {
    char p[64];
    snprintf(p, sizeof p, "/tmp/lsp_workload_lz_%d.ore", i);
    unlink_safe(p);
  }
  free(importer_text);
  free(target);
}

// === compaction-stress ==================================================
//
// Generates a procedurally-sized Ore file (N structs + N fns), opens
// it, lowers s->compact_min_threshold so the existing pool sizes
// reliably cross it within a few cycles, then runs edit-replace
// cycles. Per-iter we expect:
//
//   - pool counts grow each non-compacting cycle by ~one "delta" range.
//   - Every ~2 cycles (GROWTH_FACTOR=2), the trigger fires and a
//     pool shrinks back to its live size; n_compactions_total increments.
//   - compact_ns_delta is nonzero on compaction iters, zero otherwise.
//
// Reading the CSV you should see a clear "sawtooth" pattern in the
// pool-size columns: grow, grow, compact (drop), grow, grow, compact.
// If pools grow monotonically without dropping, compaction is broken.

// Procedurally generate `n` structs + `n` fns into `out`. Each struct
// has 3 fields; each fn has a body that does `return 0`. Realistic
// enough to populate every pool family.
static char *gen_synth_source(int n, size_t *out_len) {
  // Conservative size estimate: ~80 chars per struct + ~90 chars per fn.
  size_t cap = 256 + (size_t)n * 200;
  char *buf = (char *)malloc(cap);
  size_t off = 0;
  for (int i = 0; i < n; i++) {
    off += (size_t)snprintf(buf + off, cap - off,
                            "S%d :: struct\n"
                            "  a : i32\n"
                            "  b : i32\n"
                            "  c : i32\n",
                            i);
  }
  for (int i = 0; i < n; i++) {
    off += (size_t)snprintf(buf + off, cap - off,
                            "f%d :: fn(x : i32) i32\n"
                            "    return x\n",
                            i);
  }
  // A trivial `main` so the module has a pub entrypoint.
  off += (size_t)snprintf(buf + off, cap - off,
                          "main :: pub fn() i32\n"
                          "    return 0\n");
  buf[off] = '\0';
  if (out_len)
    *out_len = off;
  return buf;
}

static void scenario_compaction_stress(int iters, bool csv,
                                        const char *file_path) {
  (void)file_path; // we generate our own content for this scenario

  struct db db;
  db_init(&db);

  // Lower the compaction trigger so even a moderate file triggers
  // compaction within `iters` cycles. With threshold=100, any pool
  // crossing 100 entries on a body-touching edit will compact on the
  // NEXT pool re-growth past 200 (GROWTH_FACTOR=2). For a synth file
  // of N=50, body_scope_n2s is ~1500 entries after first parse; every
  // edit re-runs roughly all decls, doubling the dead set every
  // iteration.
  db.compact_min_threshold = 100;

  int n_decls = 50;
  size_t v1_len = 0;
  char *v1 = gen_synth_source(n_decls, &v1_len);
  size_t v2_len = 0;
  char *v2 = append_marker(v1, v1_len,
                            "\nCOMPACTION_STRESS_MARKER :: 0\n", &v2_len);

  const char *tmp = "/tmp/lsp_workload_compaction.ore";
  write_file_or_die(tmp, v1, v1_len);

  SourceId src = workspace_did_open(&db, tmp, strlen(tmp), v1, v1_len);
  FileId fid = db_lookup_file_by_source(&db, src);
  NamespaceId nsid = db_get_file_namespace(&db, fid);

  uint64_t prev_c = 0, prev_h = 0;
  uint64_t prev_ns = 0;
  {
    // iter 0: cold typecheck of V1
    uint64_t t0 = now_us();
    db_request_begin(&db, db_current_revision(&db));
    sema_check_module(&db, nsid);
    db_request_end(&db);
    Sample smp0;
    capture(&db, &smp0, 0, "compaction-stress", t0, prev_c, prev_h, prev_ns);
    print_sample(&smp0, csv);
    prev_c = smp0.compute_total;
    prev_h = smp0.cached_total;
    prev_ns += smp0.compact_ns_delta;
  }

  for (int i = 1; i <= iters; i++) {
    uint64_t s_us = now_us();
    const char *t = (i % 2 == 1) ? v2 : v1;
    size_t tl = (i % 2 == 1) ? v2_len : v1_len;
    workspace_did_change(&db, tmp, strlen(tmp), t, tl);
    db_request_begin(&db, db_current_revision(&db));
    sema_check_module(&db, nsid);
    db_request_end(&db);
    Sample smp;
    capture(&db, &smp, (uint32_t)i, "compaction-stress", s_us,
            prev_c, prev_h, prev_ns);
    print_sample(&smp, csv);
    prev_c = smp.compute_total;
    prev_h = smp.cached_total;
    prev_ns += smp.compact_ns_delta;
  }

#ifdef ORE_DEBUG_QUERIES
  fprintf(stderr, "\n== compaction-stress query stats ==\n");
  db_dump_query_stats(&db, stderr);
#endif

  // Summary line so a human running the scenario can eyeball the
  // result without parsing CSV.
  fprintf(stderr,
          "\n== compaction-stress summary ==\n"
          "  total compactions: bs=%" PRIu64 "  dp=%" PRIu64 "\n"
          "  bytes reclaimed:   bs=%" PRIu64 "  dp=%" PRIu64 "\n"
          "  ns in compaction:  bs=%" PRIu64 "  dp=%" PRIu64 "\n",
          db.compact_stats.n_compactions[0],
          db.compact_stats.n_compactions[1],
          db.compact_stats.bytes_reclaimed[0],
          db.compact_stats.bytes_reclaimed[1],
          db.compact_stats.total_ns[0],
          db.compact_stats.total_ns[1]);

  db_free(&db);
  free(v1);
  free(v2);
  unlink_safe(tmp);
}

// === Main ===============================================================

static void usage(void) {
  fprintf(stderr,
          "usage: lsp-workload <scenario> [--file PATH] [--iters N] "
          "[--csv] [--attach-pause]\n\n"
          "scenarios:\n"
          "  steady-typecheck  open file once; re-typecheck N times\n"
          "  edit-replace      alternate two contents N times\n"
          "  noop-edit         apply SAME text N times (hash fast-path)\n"
          "  evict-churn       open/evict cycles N times\n"
          "  cross-file        a @imports real file; edit imported N times\n"
          "  lazy-load         importer with N @imports of real file\n"
          "  compaction-stress synth file + edit cycles; lowers compact\n"
          "                    threshold so pools oscillate visibly\n"
          "  all               run every scenario sequentially\n\n"
          "default --file: examples/allocator/allocator.ore\n");
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const char *scenario = argv[1];
  const char *file_path = "examples/allocator/allocator.ore";
  int iters = 100;
  bool csv = false;
  bool attach_pause = false;

  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--file") && i + 1 < argc) {
      file_path = argv[++i];
    } else if (!strcmp(argv[i], "--iters") && i + 1 < argc) {
      iters = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--csv")) {
      csv = true;
    } else if (!strcmp(argv[i], "--attach-pause")) {
      attach_pause = true;
    } else {
      usage();
      return 2;
    }
  }

  if (attach_pause) {
    fprintf(stderr,
            "PID = %d   workload file = %s\n"
            "Attach Instruments now, then press Enter to start.\n",
            (int)getpid(), file_path);
    (void)getchar();
  }

  if (csv)
    print_csv_header();

  if (!strcmp(scenario, "steady-typecheck")) {
    scenario_steady_typecheck(iters, csv, file_path);
  } else if (!strcmp(scenario, "edit-replace")) {
    scenario_edit_replace(iters, csv, file_path);
  } else if (!strcmp(scenario, "noop-edit")) {
    scenario_noop_edit(iters, csv, file_path);
  } else if (!strcmp(scenario, "evict-churn")) {
    scenario_evict_churn(iters, csv, file_path);
  } else if (!strcmp(scenario, "cross-file")) {
    scenario_cross_file(iters, csv, file_path);
  } else if (!strcmp(scenario, "lazy-load")) {
    scenario_lazy_load(iters, csv, file_path);
  } else if (!strcmp(scenario, "compaction-stress")) {
    scenario_compaction_stress(iters, csv, file_path);
  } else if (!strcmp(scenario, "all")) {
    scenario_steady_typecheck(iters, csv, file_path);
    scenario_edit_replace(iters, csv, file_path);
    scenario_noop_edit(iters, csv, file_path);
    scenario_evict_churn(iters, csv, file_path);
    scenario_cross_file(iters, csv, file_path);
    scenario_lazy_load(iters, csv, file_path);
    scenario_compaction_stress(iters, csv, file_path);
  } else {
    usage();
    return 2;
  }
  return 0;
}
