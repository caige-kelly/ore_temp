// Lexer smoke test — reads a file from argv[1], runs lex(), dumps a
// summary plus optional token dump. Useful for checking the lexer
// against real .ore sources before the parser / layout passes land.
//
// Build:
//   cc -I src -Wall -Wextra -std=c17 -g -fsanitize=address,undefined \
//      src/lexer/token.c src/lexer/lexer.c \
//      src/db/lifecycle/lifecycle.c src/db/ids/ids.c \
//      src/db/intern_pool/intern_pool.c src/db/request/request.c \
//      src/db/workspace/module_info.c src/db/workspace/ast_id_map.c \
//      src/db/query/query.c src/db/query/query_engine.c \
//      src/db/query/invalidate.c src/db/query/ast_dep.c \
//      src/db/query/collect.c src/db/diag/diag.c \
//      src/support/data_structure/{stringpool,arena,vec,hashmap}.c \
//      tools/lex_smoke.c -o /tmp/ore-lex-smoke
//
// Run:
//   /tmp/ore-lex-smoke examples/test.ore
//   /tmp/ore-lex-smoke examples/test.ore --dump

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/db/db.h"
#include "../src/lexer/lexer.h"
#include "../src/lexer/token.h"

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    buf[n] = '\0';
    *out_len = n;
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.ore> [--dump]\n", argv[0]);
        return 1;
    }

    bool dump = (argc >= 3 && strcmp(argv[2], "--dump") == 0);

    // Read the file.
    size_t source_len = 0;
    char *source = read_file(argv[1], &source_len);
    if (!source) return 1;

    // Spin up a db, register the source, alloc a module.
    struct db s;
    db_init(&s);

    SourceId sid = db_create_source(&s, argv[1], strlen(argv[1]),
                                   source, source_len);
    (void)sid;

    // Allocate output Vecs against the request_arena (tokens, transient)
    // and a per-module-style arena (line_starts; we use request_arena
    // here since this harness doesn't go through QUERY_MODULE_AST).
    Vec tokens;
    Vec line_starts;
    vec_init_in_arena(&tokens,
                      &s.request_arena,
                      (size_t)source_len,
                      sizeof(Token));
    vec_init_in_arena(&line_starts,
                      &s.request_arena,
                      (size_t)source_len / 4 + 16,
                      sizeof(uint32_t));

    // Run the lex.
    lex(source, (uint32_t)source_len, &s.strings, &tokens, &line_starts);

    // Summary.
    size_t kinds[TK_COUNT] = {0};
    size_t errors = 0;
    for (size_t i = 0; i < tokens.count; i++) {
        Token *t = (Token *)vec_get(&tokens, i);
        if ((int)t->kind < TK_COUNT) kinds[t->kind]++;
        if (t->kind == TK_ERROR) errors++;
    }

    printf("file: %s\n", argv[1]);
    printf("  source bytes: %zu\n", source_len);
    printf("  tokens:       %zu\n", tokens.count);
    printf("  line_starts:  %zu\n", line_starts.count);
    printf("  errors:       %zu\n", errors);
    printf("\n");

    printf("token kind histogram:\n");
    for (int k = 0; k < TK_COUNT; k++) {
        if (kinds[k] == 0) continue;
        printf("  %4zu  %s\n", kinds[k], token_kind_str((TokenKind)k));
    }

    if (errors) {
        printf("\nerror tokens (byte range -> source snippet):\n");
        for (size_t i = 0; i < tokens.count; i++) {
            Token *t = (Token *)vec_get(&tokens, i);
            if (t->kind != TK_ERROR) continue;
            uint32_t start = t->byte_start;
            uint32_t end   = t->byte_end > start + 32 ? start + 32 : t->byte_end;
            uint32_t len   = end - start;
            printf("  [%u..%u] = \"%.*s\"\n",
                   t->byte_start, t->byte_end, (int)len, &source[start]);
        }
    }

    if (dump) {
        printf("\nfull token stream:\n");
        for (size_t i = 0; i < tokens.count; i++) {
            Token *t = (Token *)vec_get(&tokens, i);
            const char *kind_str = token_kind_str(t->kind);
            uint32_t len = t->byte_end - t->byte_start;

            // Don't dump huge spans verbatim; cap at 40 bytes.
            uint32_t print_len = len > 40 ? 40 : len;
            const char *lex = &source[t->byte_start];

            // Skip TK_SPACE bodies — they're just whitespace; show
            // the count instead.
            if (t->kind == TK_SPACE) {
                printf("  %4zu  %-14s [%u..%u] (%u sp)\n",
                       i, kind_str, t->byte_start, t->byte_end, len);
            } else if (t->kind == TK_NEWLINE) {
                printf("  %4zu  %-14s [%u..%u]\n",
                       i, kind_str, t->byte_start, t->byte_end);
            } else {
                printf("  %4zu  %-14s [%u..%u] \"%.*s\"%s\n",
                       i, kind_str, t->byte_start, t->byte_end,
                       (int)print_len, lex, len > 40 ? "..." : "");
            }
        }
    }

    free(source);
    db_free(&s);

    return errors == 0 ? 0 : 2;
}
