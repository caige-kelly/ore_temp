#ifndef ORE_DRIVER_OPTIONS_H
#define ORE_DRIVER_OPTIONS_H

#include <stdbool.h>

// CLI-flag struct for the batch driver. Pure surface concern — sema
// itself never reads this; it lives here so main.c has a typed home
// for argv parsing without dragging the rest of the codebase along.
//
// `use_color` is consumed at diagnostic-render time. Everything else
// gates a `--dump-*` output path or driver behavior.
struct CompilerOptions {
    const char* input_path;
    bool dump_raw;
    bool dump_lex;
    bool dump_ast;
    bool dump_resolve;
    bool dump_const_eval;
    bool quiet;
    bool use_color;
    bool help;
};

#endif
