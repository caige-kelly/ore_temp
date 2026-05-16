#include "consumers/driver/build.h"
#include "consumers/driver/options.h"
// #include "consumers/lsp/server.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(FILE *out, const char *program) {
  fprintf(out,
          "Usage: %s <subcommand> [options]\n"
          "\n"
          "Subcommands:\n"
          "  build <file>      compile and check the file\n"
          "  lsp               run as a Language Server Protocol server\n"
          "  help              show this help\n"
          "\n",
          program);
}

// Parse argv starting from the subcommand-args region (argv[2..]).
// Returns true on success, false on error (message already emitted
// to stderr). On --help, sets opts->help and returns true.
static bool parse_build_options(int argc, char **argv, const char *program,
                                struct CompilerOptions *opts) {

  for (int i = 0; i < argc; i++) {
    const char *arg = argv[i];
    if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
      print_usage(stdout, program);
      opts->help = true;
      return true;
    } else if (arg[0] == '-') {
      fprintf(stderr, "unknown option: %s\n", arg);
      print_usage(stderr, program);
      return false;
    } else if (!opts->input_path) {
      opts->input_path = arg;
    } else {
      fprintf(stderr, "unexpected extra input: %s\n", arg);
      print_usage(stderr, program);
      return false;
    }
  }

  if (!opts->input_path) {
    fprintf(stderr, "ore build: missing <file>\n");
    print_usage(stderr, program);
    return false;
  }
  return true;
}

int main(int argc, char *argv[]) {
  const char *program = argv[0];
  if (argc < 2) {
    print_usage(stderr, program);
    return EXIT_FAILURE;
  }

  const char *sub = argv[1];
  if (strcmp(sub, "build") == 0) {
    struct CompilerOptions opts = {0};
    if (!parse_build_options(argc - 2, argv + 2, program, &opts))
      return EXIT_FAILURE;
    if (opts.help)
      return EXIT_SUCCESS;
    return driver_build_run(&opts);
  }
  // if (strcmp(sub, "lsp") == 0) {
  //   return lsp_server_run();
  // }
  if (strcmp(sub, "help") == 0 || strcmp(sub, "--help") == 0 ||
      strcmp(sub, "-h") == 0) {
    print_usage(stdout, program);
    return EXIT_SUCCESS;
  }

  fprintf(stderr, "unknown subcommand: %s\n", sub);
  print_usage(stderr, program);
  return EXIT_FAILURE;
}
