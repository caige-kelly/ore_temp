#include "build.h"

#include "../common/vec.h"
#include "../diag/diag.h"
#include "../parser/parser.h"
#include "../sema/eval/dump.h"
#include "../sema/ids/ids.h"
#include "../sema/modules/inputs.h"
#include "../sema/modules/modules.h"
#include "../sema/query/query.h"
#include "../sema/resolve/dump.h"
#include "../sema/resolve/scope_index.h"
#include "../sema/sema.h"
#include "../sema/type/checker.h"
#include "../sema/type/dump.h"
#include "options.h"

#include <stdio.h>
#include <stdlib.h>

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

static void dump_ast(struct Sema *s, ModuleId mid) {
  Vec *ast = query_module_ast(s, mid);
  if (!ast)
    return;
  printf("=== ast (%zu top-level expressions) ===\n", ast->count);
  for (size_t i = 0; i < ast->count; i++) {
    struct Expr **e = (struct Expr **)vec_get(ast, i);
    if (e)
      print_ast(*e, &s->pool, 0);
  }
}

int driver_build_run(const struct CompilerOptions *opts) {
  size_t src_len = 0;
  char *src = slurp_file(opts->input_path, &src_len);
  if (!src)
    return EXIT_FAILURE;

  struct Sema sema;
  sema_init(&sema);

  InputId iid = sema_register_input(&sema, opts->input_path);
  sema_set_input_source(&sema, iid, src, src_len);
  free(src);

  ModuleId mid = module_create(&sema, iid, /*is_primitives=*/false);

  bool ok = query_module_def_map(&sema, mid);
  if (ok)
    scope_index_build_module(&sema, mid);
  // Driver-level typecheck. Pre-PR-3-Layer-0, this only ran inside
  // dump_tyck — meaning `./ore file.ore` (no flags) skipped every
  // typed-bind range-check and silently compiled overflowing values.
  // Now it runs unconditionally; dumpers stay orthogonal.
  if (ok)
    sema_check_module(&sema, mid);

  if (opts->dump_ast)
    dump_ast(&sema, mid);
  if (opts->dump_resolve)
    dump_resolve(&sema, mid);
  if (opts->dump_const_eval)
    dump_const_eval(&sema, mid);
  if (opts->dump_tyck)
    dump_tyck(&sema, mid);
  if (opts->dump_query_stats) {
#ifdef ORE_DEBUG_QUERIES
    sema_dump_query_stats(&sema, stdout);
#else
    fprintf(stderr, "--dump-query-stats: rebuild with `make debug-queries` "
                    "(ORE_DEBUG_QUERIES=1) to enable per-kind telemetry\n");
#endif
  }

  // Diagnostics live in two places now: per-slot accumulators (sema
  // queries) and the sema-global bag (parse-time / IO). Collect both
  // into a single bag (sorted by location) before rendering / setting
  // the exit code. The collector uses pass_arena so the memory dies
  // alongside this build's other transient buffers.
  struct DiagBag collected = diag_bag_new(&sema.pass_arena);
  diag_collect_all(&sema, &collected, /*file_id_filter=*/-1);
  if (diag_has_errors(&collected))
    diag_render(stderr, &collected, &sema.source_map, opts->use_color);

  int rc = (ok && !diag_has_errors(&collected)) ? EXIT_SUCCESS : EXIT_FAILURE;
  sema_free(&sema);
  return rc;
}
