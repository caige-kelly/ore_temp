#ifndef ORE_DRIVER_BUILD_H
#define ORE_DRIVER_BUILD_H

// Batch-compile driver. `ore build <file>` lands here after argv
// parsing — it slurps the input, runs the pipeline (def map →
// scope index → typecheck), executes any --dump-* flags, prints
// diagnostics, and returns an EXIT_SUCCESS / EXIT_FAILURE code.
//
// Lives in src/driver/ rather than main.c so the LSP server can
// share the option struct without dragging the batch entrypoint
// along. main.c is the dispatcher; this is one of two backends
// (the other being src/lsp/server.c).

struct CompilerOptions;

int driver_build_run(const struct CompilerOptions *opts);

#endif
