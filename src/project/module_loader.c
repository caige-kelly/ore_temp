#define _POSIX_C_SOURCE 200809L

#include "module_loader.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../compiler/compiler.h"
#include "../lexer/layout.h"
#include "../lexer/lexer.h"
#include "../lexer/token.h"
#include "../parser/parser.h"

extern char *realpath(const char *path, char *resolved_path);

static char *dup_range_in(Arena *arena, const char *s, size_t len) {
  if (len > SIZE_MAX - 1)
    return NULL;

  char *out = arena ? arena_alloc(arena, len + 1) : malloc(len + 1);
  if (!out)
    return NULL;
  memcpy(out, s, len);
  out[len] = '\0';
  return out;
}

static char *dup_cstr_in(Arena *arena, const char *s) {
  return dup_range_in(arena, s, strlen(s));
}

char *ore_read_file_to_string(const char *filepath, struct DiagBag *diags) {
  FILE *file = fopen(filepath, "r");
  if (file == NULL) {
    if (diags)
      diag_error_path(diags, filepath, "could not open file: %s",
                      strerror(errno));
    return NULL;
  }

  if (fseek(file, 0, SEEK_END) != 0) {
    int err = errno;
    if (diags)
      diag_error_path(diags, filepath, "could not seek file: %s",
                      strerror(err));
    fclose(file);
    return NULL;
  }

  long measured_size = ftell(file);
  if (measured_size < 0) {
    int err = errno;
    if (diags)
      diag_error_path(diags, filepath, "could not measure file: %s",
                      strerror(err));
    fclose(file);
    return NULL;
  }

  if ((unsigned long)measured_size > SIZE_MAX - 1) {
    if (diags)
      diag_error_path(diags, filepath, "source file is too large");
    fclose(file);
    return NULL;
  }

  if (fseek(file, 0, SEEK_SET) != 0) {
    int err = errno;
    if (diags)
      diag_error_path(diags, filepath, "could not rewind file: %s",
                      strerror(err));
    fclose(file);
    return NULL;
  }

  size_t file_size = (size_t)measured_size;
  char *source_buffer = malloc(file_size + 1);
  if (source_buffer == NULL) {
    if (diags)
      diag_error_path(diags, filepath, "could not allocate source buffer");
    fclose(file);
    return NULL;
  }

  size_t bytes_read = fread(source_buffer, 1, file_size, file);
  if (bytes_read != file_size) {
    int err = errno;
    if (diags) {
      if (ferror(file)) {
        diag_error_path(diags, filepath, "could not read file: %s",
                        strerror(err));
      } else {
        diag_error_path(diags, filepath, "could not read complete file");
      }
    }
    free(source_buffer);
    fclose(file);
    return NULL;
  }

  source_buffer[file_size] = '\0';
  if (fclose(file) != 0) {
    int err = errno;
    if (diags)
      diag_error_path(diags, filepath, "could not close file: %s",
                      strerror(err));
    free(source_buffer);
    return NULL;
  }
  return source_buffer;
}

char *ore_canonical_path(const char *filepath) {
  if (!filepath)
    return NULL;
  return realpath(filepath, NULL);
}

static bool is_absolute_path(const char *path) {
  return path && path[0] == '/';
}

static char *dirname_dup_in(Arena *arena, const char *path) {
  if (!path)
    return dup_cstr_in(arena, ".");
  const char *slash = strrchr(path, '/');
  if (!slash)
    return dup_cstr_in(arena, ".");
  if (slash == path)
    return dup_cstr_in(arena, "/");
  return dup_range_in(arena, path, (size_t)(slash - path));
}

static char *join_path_in(Arena *arena, const char *base, const char *path) {
  size_t base_len = strlen(base);
  size_t path_len = strlen(path);
  bool needs_slash = base_len > 0 && base[base_len - 1] != '/';
  size_t slash_len = needs_slash ? 1 : 0;
  if (base_len > SIZE_MAX - slash_len)
    return NULL;
  size_t prefix_len = base_len + slash_len;
  if (prefix_len > SIZE_MAX - path_len ||
      prefix_len + path_len > SIZE_MAX - 1) {
    return NULL;
  }
  size_t total_len = prefix_len + path_len + 1;
  char *out = arena ? arena_alloc(arena, total_len) : malloc(total_len);
  if (!out)
    return NULL;
  memcpy(out, base, base_len);
  size_t at = base_len;
  if (needs_slash)
    out[at++] = '/';
  memcpy(out + at, path, path_len);
  out[at + path_len] = '\0';
  return out;
}

char *ore_resolve_import_path_in(Arena *scratch_arena,
                                 const char *importer_path,
                                 const char *import_path) {
  if (!import_path)
    return NULL;

  char *candidate = NULL;
  if (is_absolute_path(import_path)) {
    candidate = dup_cstr_in(scratch_arena, import_path);
  } else {
    char *base = dirname_dup_in(scratch_arena, importer_path);
    if (!base)
      return NULL;
    candidate = join_path_in(scratch_arena, base, import_path);
    if (!scratch_arena)
      free(base);
  }

  if (!candidate)
    return NULL;
  char *canonical = ore_canonical_path(candidate);
  if (!scratch_arena)
    free(candidate);
  return canonical;
}

char *ore_resolve_import_path(const char *importer_path,
                              const char *import_path) {
  return ore_resolve_import_path_in(NULL, importer_path, import_path);
}

struct ModuleReturn *ore_parse_file(struct Compiler *compiler, const char *filepath,
                    int file_id) { 

  if (!compiler || !filepath)
    return NULL;

  StringPool *pool = &compiler->pool;
  Arena *arena = &compiler->arena;
  struct SourceMap *source_map = &compiler->source_map;
  struct DiagBag *diags = &compiler->diags;

  char *source = ore_read_file_to_string(filepath, diags);
  if (!source)
    return NULL;

  int lexer_file_id = file_id;
  if (source_map) {
    struct SourceFile *file =
        sourcemap_add_file(source_map, file_id, filepath, source);
    if (!file) {
      if (diags)
        diag_error_path(diags, filepath, "could not register source file");
      free(source);
      return NULL;
    }
    lexer_file_id = file->file_id;
    source = file->source;
  }

  struct Lexer lexer = lexer_new(source, lexer_file_id);
  Vec tokens;
  vec_init_in(&tokens, &compiler->pass_arena, sizeof(struct Token));

  struct Token token;
  for (;;) {
    token = tokenizer(&lexer, pool);
    vec_push(&tokens, &token);
    if (token.kind == Eof)
      break;
  }

  if (compiler->options.dump_raw) {
    printf("Raw Lexemes (%zu tokens):\n", tokens.count);
    for (size_t i = 0; i < tokens.count; i++) {
      struct Token *t = (struct Token *)vec_get(&tokens, i);
      if (!t) continue;
      const char *origin_str = (t->origin == Layout) ? "[L]" : "   ";
      const char *lexeme = t->string_len > 0
          ? pool_get(&compiler->pool, t->string_id, t->string_len)
          : "";
  
      printf("  %3zu: %s %4d:%-3d - %4d:%-3d   %-20s  \"",
             i, origin_str,
             t->span.line, t->span.start,
             t->span.line_end, t->span.end,
             token_kind_to_str(t->kind));
      for (const char *c = lexeme; *c; c++) {
        switch (*c) {
          case '\n': fputs("\\n",  stdout); break;
          case '\t': fputs("\\t",  stdout); break;
          case '\r': fputs("\\r",  stdout); break;
          case '"':  fputs("\\\"", stdout); break;
          default:   putchar(*c);           break;
        }
      }
      printf("\"\n");
    }
  }

  Vec *laid_out = normalizer_in(&tokens, pool, &compiler->pass_arena);

  if (compiler->options.dump_lex) {
    //Print the tokens for verification
    printf("After layout normalization (%zu tokens):\n", laid_out->count);
    for (size_t i = 0; i < laid_out->count; i++) {
        struct Token* t = (struct Token*)vec_get(laid_out, i);
        if (!t) continue;
        const char* origin_str = (t->origin == Layout) ? "[L]" : "   ";
        printf("  %3zu: %s %-20s  \"%s\"\n",
              i, origin_str,
              token_kind_to_str(t->kind),
              t->string_len > 0 ? pool_get(&compiler->pool, t->string_id, t->string_len)
              : "");
    }
  }

  struct Parser parser = parser_new_in_with_diags(laid_out, pool, arena, diags);
  Vec *ast = parse(&parser);

  if (!source_map)
    free(source);

  struct ModuleReturn *result = (struct ModuleReturn*)arena_alloc(arena, sizeof(struct ModuleReturn));
  result->ast = ast;
  result->laid_out = laid_out;
  result->tokens = &tokens;

  return result;
}
