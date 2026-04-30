#ifndef COMPILER_H
#define COMPILER_H

#include <stdbool.h>
#include <stdio.h>

#include "../common/arena.h"
#include "../common/hashmap.h"
#include "../common/stringpool.h"
#include "../common/vec.h"
#include "../diag/diag.h"
#include "../diag/sourcemap.h"
#include "../sema/target.h"

struct Module;

struct CompilerOptions {
    const char* input_path;
    bool dump_ast;
    bool dump_resolve;
    bool dump_sema;
    bool dump_effects;
    bool dump_evidence;
    bool dump_tyck;
    bool quiet;
    bool use_color;
    bool help;
};

struct Compiler {
    struct CompilerOptions options;
    StringPool pool;
    Arena arena;
    Arena scratch_arena;
    Arena pass_arena;
    struct SourceMap source_map;
    struct DiagBag diags;
    char* root_path;
    // Modules: ordered Vec for stable iteration (sema, codegen) + path-keyed
    // hashmap for import lookup. Insertion goes via the loader; both stay in
    // sync. Linear iteration is intentional — we want deterministic order.
    Vec* modules;
    HashMap* module_map;
    Vec* module_stack;
    HashMap handler_impl_decls;  // Decl* (uint64_t) -> non-null sentinel: set of decls
                                  // introduced inside a `with handler { ... }` body.
                                  // Written by name resolution, read by sema.
    struct TargetInfo target;     // build/host target — consulted by layout, const_eval, codegen
    const char* current_pass;
    int next_file_id;
    bool initialized;
};

bool compiler_init(struct Compiler* compiler, struct CompilerOptions options);
bool compiler_set_root_path(struct Compiler* compiler, const char* input_path);
void compiler_reset_scratch(struct Compiler* compiler);
void compiler_begin_pass(struct Compiler* compiler, const char* pass_name);
void compiler_end_pass(struct Compiler* compiler);
void compiler_render_diags(struct Compiler* compiler, FILE* out);
void compiler_free(struct Compiler* compiler);

#endif // COMPILER_H
