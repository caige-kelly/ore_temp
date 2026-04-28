#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include "common/vec.h"
#include "common/arena.h"
#include "common/stringpool.h"
#include "parser/parser.h"
#include "name_resolution/name_resolution.h"
#include "project/module_loader.h"
#include "sema/sema.h"
#include "diag/diag.h"
#include "diag/sourcemap.h"

struct DriverOptions {
    const char* input_path;
    bool dump_ast;
    bool dump_resolve;
    bool dump_sema;
    bool dump_effects;
    bool quiet;
    bool use_color;
    bool help;
};

static void print_usage(FILE* out, const char* program) {
    fprintf(out,
        "Usage: %s [options] <filename>\n"
        "\n"
        "Options:\n"
        "  --dump-ast      print parsed AST\n"
        "  --dump-resolve  print name-resolution dump\n"
        "  --dump-sema     print semantic/type skeleton dump\n"
        "  --dump-effects  print collected effect signatures\n"
        "  --quiet         suppress non-diagnostic status lines\n"
        "  --no-color      disable ANSI color in diagnostics\n"
        "  --help          show this help\n",
        program);
}

static bool parse_options(int argc, char** argv, struct DriverOptions* opts) {
    *opts = (struct DriverOptions){ .use_color = true };

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        if (strcmp(arg, "--dump-ast") == 0) {
            opts->dump_ast = true;
        } else if (strcmp(arg, "--dump-resolve") == 0) {
            opts->dump_resolve = true;
        } else if (strcmp(arg, "--dump-sema") == 0) {
            opts->dump_sema = true;
        } else if (strcmp(arg, "--dump-effects") == 0) {
            opts->dump_effects = true;
        } else if (strcmp(arg, "--quiet") == 0) {
            opts->quiet = true;
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
    struct DriverOptions opts;
    if (!parse_options(argc, argv, &opts)) {
        return EXIT_FAILURE;
    }
    if (opts.help) return EXIT_SUCCESS;

    char* root_path = ore_canonical_path(opts.input_path);
    if (root_path == NULL) {
        perror(opts.input_path);
        return EXIT_FAILURE;
    }

    // Initialize the string pool
    StringPool pool;
    pool_init(&pool, 1024); // Start with 1KB, will grow as needed

    Arena arena;
    arena_init(&arena, 64 * 1024 * 1024);

    struct SourceMap source_map = sourcemap_new(&arena, &pool);
    struct DiagBag diags = diag_bag_new(&arena);

    Vec* ast = ore_parse_file(root_path, &pool, &arena, 0, &source_map, &diags);
    if (!ast) {
        if (diag_has_errors(&diags)) diag_render(stderr, &diags, &source_map, opts.use_color);
        sourcemap_free_sources(&source_map);
        free(root_path);
        pool_free(&pool);
        arena_free(&arena);
        return EXIT_FAILURE;
    }

    if (opts.dump_ast) {
        printf("=== ast (%zu top-level expressions) ===\n", ast->count);
        for (size_t i = 0; i < ast->count; i++) {
            struct Expr** e = (struct Expr**)vec_get(ast, i);
            if (e) print_ast(*e, &pool, 0);
        }
    }

    // -------------------------
    // Pass 4: name resolution
    // -------------------------

    // Name resolution
    struct Resolver resolver = resolver_new(ast, &pool, &arena, root_path, &source_map, &diags);
    bool ok = resolve(&resolver);

    if (!ok) {
        if (!opts.quiet) {
            fprintf(stderr, "name resolution failed with %zu errors\n", resolver.errors->count);
        }
        diag_render(stderr, &diags, &source_map, opts.use_color);
        if (opts.dump_resolve) dump_resolution(&resolver);
        sourcemap_free_sources(&source_map);
        pool_free(&pool);
        arena_free(&arena);
        free(root_path);
        return 1;
    }

    if (opts.dump_resolve) dump_resolution(&resolver);

    // -------------------------
    // Pass 5: semantic skeleton
    // -------------------------

    struct Sema sema = sema_new(&resolver, &pool, &arena, &diags);
    bool sema_ok = sema_check(&sema);
    if (opts.dump_sema) dump_sema(&sema);
    if (opts.dump_effects) dump_sema_effects(&sema);

    if (!sema_ok) {
        if (!opts.quiet) {
            fprintf(stderr, "semantic analysis failed with %zu errors\n", sema.errors->count);
        }
        diag_render(stderr, &diags, &source_map, opts.use_color);
        sourcemap_free_sources(&source_map);
        pool_free(&pool);
        arena_free(&arena);
        free(root_path);
        return 1;
    }

    // --------------------------------------------
    // For now, we just print the tokens we found.
    // --------------------------------------------

    // //Print the tokens for verification
    // printf("After layout normalization (%zu tokens):\n", laid_out->count);
    // for (size_t i = 0; i < laid_out->count; i++) {
    //     struct Token* t = (struct Token*)vec_get(laid_out, i);
    //     if (!t) continue;
    //     const char* origin_str = (t->origin == Layout) ? "[L]" : "   ";
    //     printf("  %3zu: %s %-20s  \"%s\"\n",
    //            i, origin_str,
    //            token_kind_to_str(t->kind),
    //            t->string_len > 0 ? pool_get(&pool, t->string_id, t->string_len) : "");
    // }

    // Clean up
    sourcemap_free_sources(&source_map);
    pool_free(&pool);
    arena_free(&arena);
    free(root_path);

    return EXIT_SUCCESS;
}
