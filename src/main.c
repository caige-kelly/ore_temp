#include "compiler/compiler.h"
#include "hir/dump.h"
#include "parser/parser.h"
#include "project/module_loader.h"
#include "sema/type/effect_solver.h"
#include "sema/sema.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(FILE *out, const char *program) {
  fprintf(out,
          "Usage: %s [options] <filename>\n"
          "\n"
          "Options:\n"
          "  --dump-ast      print parsed AST\n"
          "  --dump-resolve  print name-resolution dump\n"
          "  --dump-sema     print semantic/type skeleton dump\n"
          "  --dump-effects  print collected effect signatures\n"
          "  --dump-evidence print evidence vectors per body and per call\n"
          "  --dump-tyck     print collected type signatures\n"
          "  --dump-hir      print lowered HIR per function\n"
          "  --dump-lex      print normalized lexer output\n"
          "  --quiet         suppress non-diagnostic status lines\n"
          "  --no-color      disable ANSI color in diagnostics\n"
          "  --help          show this help\n",
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
    } else if (strcmp(arg, "--dump-sema") == 0) {
      opts->dump_sema = true;
    } else if (strcmp(arg, "--dump-effects") == 0) {
      opts->dump_effects = true;
    } else if (strcmp(arg, "--dump-evidence") == 0) {
      opts->dump_evidence = true;
    } else if (strcmp(arg, "--quiet") == 0) {
      opts->quiet = true;
    } else if (strcmp(arg, "--dump-tyck") == 0) {
      opts->dump_tyck = true;
    } else if (strcmp(arg, "--dump-hir") == 0) {
      opts->dump_hir = true;
    } else if (strcmp(arg, "--dump-lex") == 0) {
      opts->dump_lex = true;
    } else if (strcmp(arg, "--dump-raw") == 0) {
      opts->dump_raw = true;
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

int main(int argc, char *argv[]) {
  // ----------------------------------------------
  // Pass 0: Read the source file(s) into a string.
  // -----------------------------------------------
  struct CompilerOptions opts;
  if (!parse_options(argc, argv, &opts)) {
    return EXIT_FAILURE;
  }
  if (opts.help)
    return EXIT_SUCCESS;

  struct Compiler compiler;
  if (!compiler_init(&compiler, opts)) {
    return EXIT_FAILURE;
  }
  if (!compiler_set_root_path(&compiler, opts.input_path)) {
    compiler_render_diags(&compiler, stderr);
    compiler_free(&compiler);
    return EXIT_FAILURE;
  }

  compiler_begin_pass(&compiler, "parse");
  struct ModuleReturn *module_parsed = ore_parse_file(&compiler, compiler.root_path, 0);

  Vec *ast = module_parsed->ast;

  compiler_end_pass(&compiler);
  if ((!ast || diag_has_errors(&compiler.diags)) && !opts.quiet) {
    if (diag_has_errors(&compiler.diags))
      compiler_render_diags(&compiler, stderr);
    compiler_free(&compiler);
    return EXIT_FAILURE;
  }

  if (opts.dump_ast) {
    printf("=== ast (%zu top-level expressions) ===\n", ast->count);
    for (size_t i = 0; i < ast->count; i++) {
      struct Expr **e = (struct Expr **)vec_get(ast, i);
      if (e)
        print_ast(*e, &compiler.pool, 0);
    }
  }

  // Sema/HIR pipeline is being rebuilt on top of the new query-based
  // resolution layer. The driver wiring lands when the un-prune
  // cleanup connects the new sema_check to scope_index + resolve.

  compiler_free(&compiler);
  return EXIT_SUCCESS;
}
