#include "compiler.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "../project/module_loader.h"

enum {
    COMPILER_ARENA_SIZE = 64 * 1024 * 1024,
    COMPILER_SCRATCH_ARENA_SIZE = 8 * 1024 * 1024,
    COMPILER_PASS_ARENA_SIZE = 16 * 1024 * 1024,
};

bool compiler_init(struct Compiler* compiler, struct CompilerOptions options) {
    if (!compiler) return false;

    *compiler = (struct Compiler){0};
    compiler->options = options;

    pool_init(&compiler->pool, 1024);
    arena_init(&compiler->arena, COMPILER_ARENA_SIZE);
    arena_init(&compiler->scratch_arena, COMPILER_SCRATCH_ARENA_SIZE);
    arena_init(&compiler->pass_arena, COMPILER_PASS_ARENA_SIZE);
    compiler->source_map = sourcemap_new(&compiler->arena, &compiler->pool);
    compiler->diags = diag_bag_new(&compiler->arena);
    compiler->modules = vec_new_in(&compiler->arena, sizeof(struct Module*));
    compiler->module_stack = vec_new_in(&compiler->arena, sizeof(struct Module*));
    compiler->next_file_id = 1;
    compiler->initialized = true;
    return true;
}

void compiler_reset_scratch(struct Compiler* compiler) {
    if (!compiler || !compiler->initialized) return;
    arena_reset(&compiler->scratch_arena);
}

void compiler_begin_pass(struct Compiler* compiler, const char* pass_name) {
    if (!compiler || !compiler->initialized) return;
    compiler->current_pass = pass_name;
    arena_reset(&compiler->pass_arena);
}

void compiler_end_pass(struct Compiler* compiler) {
    if (!compiler || !compiler->initialized) return;
    compiler->current_pass = NULL;
}

bool compiler_set_root_path(struct Compiler* compiler, const char* input_path) {
    if (!compiler || !compiler->initialized) return false;

    compiler->root_path = ore_canonical_path(input_path);
    if (!compiler->root_path) {
        diag_error_path(&compiler->diags, input_path,
            "could not resolve path: %s", strerror(errno));
        return false;
    }
    return true;
}

void compiler_render_diags(struct Compiler* compiler, FILE* out) {
    if (!compiler) return;
    diag_render(out, &compiler->diags, &compiler->source_map,
        compiler->options.use_color);
}

void compiler_free(struct Compiler* compiler) {
    if (!compiler || !compiler->initialized) return;

    sourcemap_free_sources(&compiler->source_map);
    free(compiler->root_path);
    compiler->root_path = NULL;
    pool_free(&compiler->pool);
    arena_free(&compiler->pass_arena);
    arena_free(&compiler->scratch_arena);
    arena_free(&compiler->arena);
    compiler->initialized = false;
}
