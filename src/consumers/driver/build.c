// CLI build driver — `./ore build foo.ore`. A thin translator over
// the canonical compile_file pipeline: slurp source from disk, route
// through workspace_did_open + compile_file, print diagnostics to
// stderr, optionally dump AST/sema for debugging. All actual parse +
// sema + diag-collection work lives in src/compiler/compile.c, shared
// with the LSP server (and any future consumer).

#include "../../compiler/compile.h"
#include "../../db/db.h"
#include "../../db/diag/diag.h"
#include "../../db/workspace/workspace.h"
#include "../../support/data_structure/vec.h"
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
  if (!src)
    return EXIT_FAILURE;

  struct db db;
  db_init(&db);

  // Register the source via the workspace coordinator (it canonicalizes
  // the path via realpath, dedupes by canonical path, allocates fresh
  // SourceId / NamespaceId / FileId for first-time admits). The LSP
  // routes through the same entry point — same lazy-import semantics
  // for cross-file @imports either way.
  SourceId sid = workspace_did_open(&db, opts->input_path,
                                    strlen(opts->input_path), src, src_len);

  // ORE_PROFILE_LOOP=N — debug knob for the profile loop in
  // compile_file. >1 re-parses the file N times for perf sampling.
  // compile_file handles the slot reset + diag_clear between iters
  // (the driver used to do this open-coded and missed db_diags_clear,
  // which caused stale parse diags to accumulate across iterations).
  CompileFileOpts co = {.profile_count = 1};
  const char *lp = getenv("ORE_PROFILE_LOOP");
  if (lp) {
    int n = atoi(lp);
    if (n > 1)
      co.profile_count = n;
  }

  Vec diags;
  vec_init(&diags, sizeof(Diag));
  FileId fid = compile_file(&db, sid, &co, &diags);

  DiagResolver dr;
  diag_resolver_init(&dr, &db);
  size_t errors = 0;
  for (size_t i = 0; i < diags.count; i++) {
    Diag *d = (Diag *)vec_get(&diags, i);
    diag_resolver_print(&dr, d, stderr);
    if (d->severity == DIAG_ERROR)
      errors++;
  }
  diag_resolver_free(&dr);

  if (!opts->quiet) {
    if (errors == 0)
      printf("OK: %s\n", opts->input_path);
    else
      printf("FAIL: %s — %zu error(s)\n", opts->input_path, errors);
  }

  (void)fid;
  vec_free(&diags);
  db_free(&db);
  free(src);
  return errors == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
