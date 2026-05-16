// Throwaway: dump the post-layout real-token stream so we can see
// exactly where synthetic `;`/`{`/`}` land. Not part of the build.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/db/db.h"
#include "../src/lexer/lexer.h"
#include "../src/lexer/layout.h"
#include "../src/lexer/token.h"

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f); buf[n] = 0; *out_len = n; return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <file.ore>\n", argv[0]); return 1; }
    size_t slen = 0;
    char *src = read_file(argv[1], &slen);
    if (!src) return 1;

    struct db s; db_init(&s);

    Vec raw, lines, real, triv, troff;
    vec_init(&raw, sizeof(Token));
    vec_init(&lines, sizeof(uint32_t));
    vec_init(&real, sizeof(Token));
    vec_init(&triv, sizeof(Token));
    vec_init(&troff, sizeof(uint32_t));

    lex(src, slen, &s.strings, &raw, &lines);
    layout(&raw, lines.data, lines.count, &real, &triv, &troff);

    printf("post-layout real tokens (%zu):\n", real.count);
    for (size_t i = 0; i < real.count; i++) {
        Token *t = vec_get(&real, i);
        int synth = (t->start == t->byte_end);
        printf("  %3zu  %-14s%s", i, token_kind_str(t->kind),
               synth ? "  (synthetic)" : "");
        if (!synth && t->byte_end > t->start) {
            uint32_t len = t->byte_end - t->start;
            if (len > 24) len = 24;
            printf("  \"%.*s\"", (int)len, &src[t->start]);
        }
        printf("\n");
    }
    return 0;
}
