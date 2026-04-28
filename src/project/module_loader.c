#define _POSIX_C_SOURCE 200809L

#include "module_loader.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../lexer/layout.h"
#include "../lexer/lexer.h"
#include "../lexer/token.h"
#include "../parser/parser.h"

extern char* realpath(const char* path, char* resolved_path);

static char* dup_range(const char* s, size_t len) {
    char* out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

static char* dup_cstr(const char* s) {
    return dup_range(s, strlen(s));
}

char* ore_read_file_to_string(const char* filepath) {
    FILE* file = fopen(filepath, "r");
    if (file == NULL) {
        perror(filepath);
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    rewind(file);

    char* source_buffer = malloc((size_t)file_size + 1);
    if (source_buffer == NULL) {
        fprintf(stderr, "Could not allocate memory for file: %s\n", filepath);
        fclose(file);
        return NULL;
    }

    size_t bytes_read = fread(source_buffer, 1, (size_t)file_size, file);
    if (bytes_read != (size_t)file_size) {
        fprintf(stderr, "Error reading file: %s\n", filepath);
        free(source_buffer);
        fclose(file);
        return NULL;
    }

    source_buffer[file_size] = '\0';
    fclose(file);
    return source_buffer;
}

char* ore_canonical_path(const char* filepath) {
    if (!filepath) return NULL;
    return realpath(filepath, NULL);
}

static bool is_absolute_path(const char* path) {
    return path && path[0] == '/';
}

static char* dirname_dup(const char* path) {
    if (!path) return dup_cstr(".");
    const char* slash = strrchr(path, '/');
    if (!slash) return dup_cstr(".");
    if (slash == path) return dup_cstr("/");
    return dup_range(path, (size_t)(slash - path));
}

static char* join_path(const char* base, const char* path) {
    size_t base_len = strlen(base);
    size_t path_len = strlen(path);
    bool needs_slash = base_len > 0 && base[base_len - 1] != '/';
    char* out = malloc(base_len + (needs_slash ? 1 : 0) + path_len + 1);
    if (!out) return NULL;
    memcpy(out, base, base_len);
    size_t at = base_len;
    if (needs_slash) out[at++] = '/';
    memcpy(out + at, path, path_len);
    out[at + path_len] = '\0';
    return out;
}

char* ore_resolve_import_path(const char* importer_path, const char* import_path) {
    if (!import_path) return NULL;

    char* candidate = NULL;
    if (is_absolute_path(import_path)) {
        candidate = dup_cstr(import_path);
    } else {
        char* base = dirname_dup(importer_path);
        if (!base) return NULL;
        candidate = join_path(base, import_path);
        free(base);
    }

    if (!candidate) return NULL;
    char* canonical = ore_canonical_path(candidate);
    free(candidate);
    return canonical;
}

Vec* ore_parse_file(const char* filepath, StringPool* pool, Arena* arena, int file_id,
                    struct SourceMap* source_map, struct DiagBag* diags) {
    char* source = ore_read_file_to_string(filepath);
    if (!source) return NULL;

    int lexer_file_id = file_id;
    if (source_map) {
        struct SourceFile* file = sourcemap_add_file(source_map, file_id, filepath, source);
        if (!file) {
            free(source);
            return NULL;
        }
        lexer_file_id = file->file_id;
        source = file->source;
    }

    struct Lexer lexer = lexer_new(source, lexer_file_id);
    Vec tokens;
    vec_init(&tokens, sizeof(struct Token));

    struct Token token;
    for (;;) {
        token = tokenizer(&lexer, pool);
        vec_push(&tokens, &token);
        if (token.kind == Eof) break;
    }

    Vec* laid_out = normalizer(&tokens, pool);
    struct Parser parser = parser_new_in_with_diags(laid_out, pool, arena, diags);
    Vec* ast = parse(&parser);

    vec_free(laid_out);
    free(laid_out);
    vec_free(&tokens);
    if (!source_map) free(source);

    return ast;
}
