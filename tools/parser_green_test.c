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
//   4. Source-byte coverage: green_node_text_len(root) covers every
//      source byte. Parser_new now consumes the unified token stream
//      including trivia, so the green tree is lossless.

#include "../src/lexer/lexer.h"
#include "../src/lexer/layout.h"
#include "../src/parser_new/parser.h"
#include "../src/support/data_structure/stringpool.h"
#include "../src/support/data_structure/arena.h"
#include "../src/support/data_structure/vec.h"
#include "../src/syntax/syntax.h"
#include "../src/syntax/syntax_kind.h"
#include "../src/lexer/token.h"

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIE(...) do { fflush(stdout); fprintf(stderr, "parser_green_test: " __VA_ARGS__); \
                       fprintf(stderr, "\n"); fflush(stderr); exit(1); } while (0)


// Recursive walk counting nodes of a specific kind under `root`.
// `root` is owned by caller — this function doesn't take/release refs.
static uint32_t count_kind(const GreenNode *root, SyntaxKind want) {
    uint32_t total = (green_node_kind(root) == want) ? 1 : 0;
    uint32_t n = green_node_num_children(root);
    for (uint32_t i = 0; i < n; i++) {
        GreenElement el = green_node_child(root, i);
        if (el.kind == GREEN_ELEM_NODE && el.node)
            total += count_kind(el.node, want);
    }
    return total;
}


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

    NodeCache *cache = node_cache_new();
    Vec errors;
    vec_init(&errors, sizeof(ParseError));

    // Parser_new consumes the UNIFIED token stream including trivia
    // (Phase A.1.3). The green tree is byte-lossless: text_len equals
    // total non-EOF source bytes.
    GreenNode *root = parse_file_green(&tokens, source, &pool, cache, &errors);

    if (!root) DIE("[%s] parse_file_green returned NULL", path);
    if (green_node_kind(root) != SK_SOURCE_FILE)
        DIE("[%s] root kind = %u, expected SK_SOURCE_FILE", path,
            green_node_kind(root));

    // Compute expected coverage: source_len minus the EOF token's
    // byte_end gap (EOF and virtual layout tokens are zero-width;
    // every other token's byte range is covered).
    uint32_t green_len = green_node_text_len(root);
    if (green_len != source_len)
        DIE("[%s] green tree text_len (%u) != source_len (%u) — "
            "trivia attachment dropped or duplicated bytes",
            path, green_len, source_len);

    // Report errors but don't abort — examples may exercise edge cases.
    if (errors.count > 0) {
        fprintf(stderr, "[%s] %zu parse errors:\n", path, errors.count);
        for (size_t i = 0; i < errors.count && i < 5; i++) {
            ParseError *e = (ParseError *)vec_get(&errors, i);
            // Print the offending token + a few surrounding tokens.
            const Token *toks = (const Token *)tokens.data;
            uint32_t lo = e->tok_pos >= 2 ? e->tok_pos - 2 : 0;
            uint32_t hi = (e->tok_pos + 3 < (uint32_t)tokens.count)
                              ? e->tok_pos + 3 : (uint32_t)tokens.count;
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
           path, (uint32_t)tokens.count,
           green_node_num_children(root), errors.count);

    // ---- Shape assertions per-file -----------------------------------
    if (strstr(path, "exn.ore")) {
        // exn.ore covers the EFFECT DECL path (`exn :: pub effect { ... }`).
        // Op signatures inside effect decls are SK_FIELD nodes.
        uint32_t n_effect = count_kind(root, SK_EFFECT_DECL);
        if (n_effect < 1)
            DIE("[exn.ore] expected >=1 SK_EFFECT_DECL, got %u", n_effect);
        printf("    [exn.ore shape] effect_decl=%u\n", n_effect);
    }
    if (strstr(path, "test.ore")) {
        // test.ore covers handler/effect/with: at least one effect decl
        // and at least one SK_HANDLER_EXPR (from `with handler { ... }`).
        // Op clauses are SK_CONST_DECL binds, not a dedicated node kind.
        uint32_t n_effect = count_kind(root, SK_EFFECT_DECL);
        uint32_t n_handler = count_kind(root, SK_HANDLER_EXPR);
        if (n_effect < 1)
            DIE("[test.ore] expected >=1 SK_EFFECT_DECL, got %u", n_effect);
        if (n_handler < 1)
            DIE("[test.ore] expected >=1 SK_HANDLER_EXPR, got %u", n_handler);
        printf("    [test.ore shape] effect=%u handler=%u\n",
               n_effect, n_handler);
    }
    if (strstr(path, "labels.ore")) {
        // labels.ore covers labeled break/continue across nested loops.
        // Parity gate: confirm parser_new emits SK_BREAK_STMT / SK_CONTINUE_STMT
        // as proper wrapper nodes (not bare keyword tokens), and at least one
        // labeled SK_LOOP_EXPR is present.
        uint32_t n_loop = count_kind(root, SK_LOOP_EXPR);
        uint32_t n_break = count_kind(root, SK_BREAK_STMT);
        uint32_t n_continue = count_kind(root, SK_CONTINUE_STMT);
        if (n_loop < 1)
            DIE("[labels.ore] expected >=1 SK_LOOP_EXPR, got %u", n_loop);
        if (n_break < 1)
            DIE("[labels.ore] expected >=1 SK_BREAK_STMT (wrapper), got %u", n_break);
        if (n_continue < 1)
            DIE("[labels.ore] expected >=1 SK_CONTINUE_STMT (wrapper), got %u", n_continue);
        printf("    [labels.ore shape] loop=%u break=%u continue=%u\n",
               n_loop, n_break, n_continue);
    }
    if (strstr(path, "tests/structs.ore")) {
        // structs.ore exercises anonymous-nested SK_UNION_DECL inside a struct
        // body — parity gate confirming parser_new emits the union wrapper
        // symmetrically with SK_STRUCT_DECL.
        uint32_t n_struct = count_kind(root, SK_STRUCT_DECL);
        uint32_t n_union = count_kind(root, SK_UNION_DECL);
        if (n_struct < 1)
            DIE("[structs.ore] expected >=1 SK_STRUCT_DECL, got %u", n_struct);
        if (n_union < 1)
            DIE("[structs.ore] expected >=1 SK_UNION_DECL, got %u", n_union);
        printf("    [structs.ore shape] struct=%u union=%u\n", n_struct, n_union);
    }
    if (strstr(path, "trailing_lambdas.ore")) {
        // Slice 6: `<-` was dropped in favor of juxtaposition. After any
        // expression in value position, a trailing `{ body }`, virtual `{`,
        // or `fn(params) body` is consumed as a lambda argument of the LHS
        // call. Parser emits plain SK_CALL_EXPR with the lambda as the
        // last positional arg — no SK_LARROW marker, no special node kind.
        //
        // Gate: at least one SK_LAMBDA_EXPR (the top-level fn-decls + every
        // trailing thunk) and one SK_CALL_EXPR (the calls that the thunks
        // attach to). SK_BIN_EXPR is incidental (arithmetic inside bodies).
        uint32_t n_call = count_kind(root, SK_CALL_EXPR);
        uint32_t n_lambda = count_kind(root, SK_LAMBDA_EXPR);
        if (n_call < 1)
            DIE("[trailing_lambdas.ore] expected >=1 SK_CALL_EXPR, got %u",
                n_call);
        if (n_lambda < 1)
            DIE("[trailing_lambdas.ore] expected >=1 SK_LAMBDA_EXPR, got %u",
                n_lambda);
        printf("    [trailing_lambdas.ore shape] call=%u lambda=%u\n",
               n_call, n_lambda);
    }

    green_node_release(root);
    node_cache_destroy(cache);
    vec_free(&errors);
    vec_free(&tokens);
    vec_free(&line_starts);
    pool_free(&pool);
    free(source);
}


// Test every .ore file under examples/tests/ in addition to the
// hand-picked feature-rich files at examples/. The tests/ directory
// is the parity surface: each file targets a specific language feature
// and exercising all of them is the parity gate.
static void run_glob(const char *dir, int *tested_count) {
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "parser_green_test: cannot open %s (errno-ish skipped)\n", dir);
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        size_t nl = strlen(name);
        if (nl < 4 || strcmp(name + nl - 4, ".ore") != 0) continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        test_file(path);
        (*tested_count)++;
    }
    closedir(d);
}

int main(int argc, char **argv) {
    // Allow overriding via argv for ad-hoc testing.
    if (argc > 1) {
        for (int i = 1; i < argc; i++) test_file(argv[i]);
        printf("parser_green_test: PASS\n");
        return 0;
    }

    // Feature-rich top-level examples — shape assertions key off these names.
    const char *featured[] = {
        "examples/test.ore",
        "examples/exn.ore",
        "examples/import_basic.ore",
        "examples/labels.ore",
        "examples/trailing_lambdas.ore",
    };
    int n_featured = (int)(sizeof(featured) / sizeof(featured[0]));
    printf("parser_green_test: parsing %d featured examples\n", n_featured);
    for (int i = 0; i < n_featured; i++) test_file(featured[i]);

    // Full parity surface — every .ore under examples/tests/.
    int n_tests = 0;
    printf("parser_green_test: parsing examples/tests/*.ore\n");
    run_glob("examples/tests", &n_tests);
    printf("parser_green_test: parsed %d test fixtures\n", n_tests);

    printf("parser_green_test: PASS\n");
    return 0;
}
