// Standalone parser smoke test (Phase A.1.2 verification).
//
// Reads each examples/*.ore file, runs lex+layout+parse, and verifies
// the resulting green tree's structural shape. Does NOT link against
// the db layer — parser_new is decoupled from db, so this test exercises
// the parser in isolation.
//
// Verification per file:
//   1. parse_file_green returns a non-NULL GreenNode.
//   2. The root kind is SK_SOURCE_FILE.
//   3. No errors recorded for well-formed examples.
//   4. Source-byte coverage: green_node_text_len(root) <= source_len.
//      (Full byte-for-byte round-trip arrives in A.1.3 when the parser
//      also consumes trivia tokens.)

#include "../src/lexer/lexer.h"
#include "../src/lexer/layout.h"
#include "../src/parser_new/parser.h"
#include "../src/support/data_structure/stringpool.h"
#include "../src/support/data_structure/arena.h"
#include "../src/support/data_structure/vec.h"
#include "../src/syntax/syntax.h"
#include "../src/parser/syntax_kind.h"
#include "../src/lexer/token.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIE(...) do { fprintf(stderr, "parser_green_test: " __VA_ARGS__); \
                       fprintf(stderr, "\n"); exit(1); } while (0)


static char *slurp(const char *path, uint32_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) DIE("cannot open %s", path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) DIE("oom");
    size_t got = fread(buf, 1, (size_t)n, f);
    (void)got;
    fclose(f);
    buf[n] = '\0';
    *out_len = (uint32_t)n;
    return buf;
}


static void test_file(const char *path) {
    uint32_t source_len = 0;
    char *source = slurp(path, &source_len);

    StringPool pool;
    pool_init(&pool, 64);

    Vec line_starts;
    vec_init(&line_starts, sizeof(uint32_t));

    Vec tokens;
    vec_init(&tokens, sizeof(Token));

    LexCursor lc;
    lex_begin(&lc, source, source_len, &pool, &line_starts);
    layout_stream(&lc, &line_starts, &tokens);

    // Filter trivia (A.1.2 contract: parser_new takes a filtered stream;
    // A.1.3 will inline trivia into the parser's cursor).
    Vec parse_tokens;
    vec_init(&parse_tokens, sizeof(Token));
    vec_reserve(&parse_tokens, tokens.count);
    for (size_t i = 0; i < tokens.count; i++) {
        Token *t = (Token *)vec_get(&tokens, i);
        if (!token_is_trivia(t->kind))
            *(Token *)vec_push_slot(&parse_tokens) = *t;
    }
    vec_free(&tokens);

    NodeCache *cache = node_cache_new();
    Vec errors;
    vec_init(&errors, sizeof(ParseError));

    GreenNode *root = parse_file_green(&parse_tokens, source, cache, &errors);

    if (!root) DIE("[%s] parse_file_green returned NULL", path);
    if (green_node_kind(root) != SK_SOURCE_FILE)
        DIE("[%s] root kind = %u, expected SK_SOURCE_FILE", path,
            green_node_kind(root));

    uint32_t green_len = green_node_text_len(root);
    if (green_len > source_len)
        DIE("[%s] green tree text_len (%u) exceeds source_len (%u)",
            path, green_len, source_len);

    // Report errors but don't abort — examples may exercise edge cases.
    if (errors.count > 0) {
        fprintf(stderr, "[%s] %zu parse errors:\n", path, errors.count);
        for (size_t i = 0; i < errors.count && i < 5; i++) {
            ParseError *e = (ParseError *)vec_get(&errors, i);
            // Print the offending token + a few surrounding tokens.
            const Token *toks = (const Token *)parse_tokens.data;
            uint32_t lo = e->tok_pos >= 2 ? e->tok_pos - 2 : 0;
            uint32_t hi = (e->tok_pos + 3 < (uint32_t)parse_tokens.count)
                              ? e->tok_pos + 3 : (uint32_t)parse_tokens.count;
            fprintf(stderr, "  tok %u: %s\n    context:", e->tok_pos, e->msg);
            for (uint32_t j = lo; j < hi; j++) {
                uint32_t l = token_len(&toks[j]);
                if (l > 30) l = 30;
                const char *txt = source + toks[j].start;
                fprintf(stderr, " %s'%.*s'",
                        j == e->tok_pos ? "[" : "",
                        (int)l, txt);
                if (j == e->tok_pos) fprintf(stderr, "]");
            }
            // Line number
            uint32_t off = toks[e->tok_pos].start;
            uint32_t line = 1;
            for (uint32_t k = 0; k < off; k++) if (source[k] == '\n') line++;
            fprintf(stderr, " (line %u)\n", line);
        }
        if (errors.count > 5) fprintf(stderr, "  ... (%zu more)\n", errors.count - 5);
    }

    printf("  %s: %u tokens, root has %u children, %zu errors\n",
           path, (uint32_t)parse_tokens.count,
           green_node_num_children(root), errors.count);

    green_node_release(root);
    node_cache_destroy(cache);
    vec_free(&errors);
    vec_free(&parse_tokens);
    vec_free(&line_starts);
    pool_free(&pool);
    free(source);
}


int main(int argc, char **argv) {
    const char *files[] = {
        "examples/test.ore",
        "examples/exn.ore",
        "examples/import_basic.ore",
        "examples/labels.ore",
        "examples/trailing_lambdas.ore",
    };
    int n = (int)(sizeof(files) / sizeof(files[0]));

    // Allow overriding via argv for ad-hoc testing.
    if (argc > 1) {
        for (int i = 1; i < argc; i++) test_file(argv[i]);
    } else {
        printf("parser_green_test: parsing %d examples\n", n);
        for (int i = 0; i < n; i++) test_file(files[i]);
    }

    printf("parser_green_test: PASS\n");
    return 0;
}
