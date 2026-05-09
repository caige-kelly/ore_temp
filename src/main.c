#include "common/vec.h"
#include "diag/diag.h"
#include "driver/options.h"
#include "parser/parser.h"
#include "sema/eval/dump.h"
#include "sema/ids/ids.h"
#include "sema/modules/inputs.h"
#include "sema/modules/modules.h"
#include "sema/query/query.h"
#include "sema/resolve/dump.h"
#include "sema/resolve/scope_index.h"
#include "sema/sema.h"
#include "sema/type/checker.h"
#include "sema/type/dump.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(FILE *out, const char *program) {
  fprintf(out,
          "Usage: %s [options] <filename>\n"
          "\n"
          "Options:\n"
          "  --dump-ast         print parsed AST\n"
          "  --dump-resolve     print top-level def map + per-Ident resolution\n"
          "  --dump-const-eval  print evaluated constants for top-level binds\n"
          "  --dump-tyck        print typecheck results (decl types + fits-in)\n"
          "  --dump-lex         print normalized lexer output\n"
          "  --quiet            suppress non-diagnostic status lines\n"
          "  --no-color         disable ANSI color in diagnostics\n"
          "  --help             show this help\n",
          program);
}

static bool parse_options(int argc, char **argv, struct CompilerOptions *opts) {
  *opts = (struct CompilerOptions){.use_color = true};

  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    if (strcmp(arg, "--dump-ast") == 0) {
      opts->dump_ast = true;
    } else if (strcmp(arg, "--dump-resolve") == 0) {
      opts->dump_resolve = true;
    } else if (strcmp(arg, "--quiet") == 0) {
      opts->quiet = true;
    } else if (strcmp(arg, "--dump-lex") == 0) {
      opts->dump_lex = true;
    } else if (strcmp(arg, "--dump-raw") == 0) {
      opts->dump_raw = true;
    } else if (strcmp(arg, "--dump-const-eval") == 0) {
      opts->dump_const_eval = true;
    } else if (strcmp(arg, "--dump-tyck") == 0) {
      opts->dump_tyck = true;
    } else if (strcmp(arg, "--no-color") == 0) {
      opts->use_color = false;
    } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
      print_usage(stdout, argv[0]);
      opts->help = true;
      return true;
    } else if (arg[0] == '-') {
      fprintf(stderr, "unknown option: %s\n", arg);
      print_usage(stderr, argv[0]);
      return false;
    } else if (!opts->input_path) {
      opts->input_path = arg;
    } else {
      fprintf(stderr, "unexpected extra input: %s\n", arg);
      print_usage(stderr, argv[0]);
      return false;
    }
  }

  if (!opts->input_path) {
    print_usage(stderr, argv[0]);
    return false;
  }
  return true;
}

// Read a file in full into a malloc'd buffer (caller frees). Returns
// NULL and prints an error on failure.
static char *slurp_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "could not open %s\n", path);
    return NULL;
  }
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  long sz = ftell(f);
  if (sz < 0) { fclose(f); return NULL; }
  rewind(f);
  char *buf = (char *)malloc((size_t)sz + 1);
  if (!buf) { fclose(f); return NULL; }
  size_t n = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  if (n != (size_t)sz) { free(buf); return NULL; }
  buf[sz] = '\0';
  if (out_len) *out_len = (size_t)sz;
  return buf;
}

static void dump_ast(struct Sema *s, ModuleId mid) {
  Vec *ast = query_module_ast(s, mid);
  if (!ast) return;
  printf("=== ast (%zu top-level expressions) ===\n", ast->count);
  for (size_t i = 0; i < ast->count; i++) {
    struct Expr **e = (struct Expr **)vec_get(ast, i);
    if (e) print_ast(*e, &s->pool, 0);
  }
}

int main(int argc, char *argv[]) {
  struct CompilerOptions opts;
  if (!parse_options(argc, argv, &opts))
    return EXIT_FAILURE;
  if (opts.help)
    return EXIT_SUCCESS;

  size_t src_len = 0;
  char *src = slurp_file(opts.input_path, &src_len);
  if (!src)
    return EXIT_FAILURE;

  struct Sema sema;
  sema_init(&sema);

  // ---- Pipeline ----
  InputId iid = sema_register_input(&sema, opts.input_path);
  sema_set_input_source(&sema, iid, src, src_len);
  free(src);

  ModuleId mid = module_create(&sema, iid, /*is_primitives=*/false);

  bool ok = query_module_def_map(&sema, mid);
  if (ok) scope_index_build_module(&sema, mid);
  // Driver-level typecheck. Pre-PR-3-Layer-0, this only ran inside
  // dump_tyck — meaning `./ore file.ore` (no flags) skipped every
  // typed-bind range-check and silently compiled overflowing values.
  // Now it runs unconditionally; dumpers stay orthogonal.
  if (ok) sema_check_module(&sema, mid);

  // ---- Dumpers ----
  if (opts.dump_ast)        dump_ast(&sema, mid);
  if (opts.dump_resolve)    dump_resolve(&sema, mid);
  if (opts.dump_const_eval) dump_const_eval(&sema, mid);
  if (opts.dump_tyck)       dump_tyck(&sema, mid);

  if (diag_has_errors(&sema.diags))
    diag_render(stderr, &sema.diags, &sema.source_map, opts.use_color);

  int rc = (ok && !diag_has_errors(&sema.diags)) ? EXIT_SUCCESS : EXIT_FAILURE;
  sema_free(&sema);
  return rc;
}
