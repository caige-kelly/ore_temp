// Throwaway: parse a .ore file and pretty-print the green tree with
// indentation, skipping trivia. Usage: ast_dump <file.ore>
//
// Decoupled from db (parser_new is). Mirrors parser_green_test's setup.

#include "../src/lexer/lexer.h"
#include "../src/lexer/layout.h"
#include "../src/lexer/token.h"
#include "../src/parser_new/parser.h"
#include "../src/support/data_structure/stringpool.h"
#include "../src/support/data_structure/vec.h"
#include "../src/syntax/syntax.h"
#include "../src/syntax/syntax_kind.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *path, uint32_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = (char *)malloc((size_t)n + 1);
  size_t got = fread(buf, 1, (size_t)n, f);
  (void)got;
  fclose(f);
  buf[n] = '\0';
  *out_len = (uint32_t)n;
  return buf;
}

static void print_tree(const GreenNode *node, int depth) {
  for (int i = 0; i < depth; i++) fputs("  ", stdout);
  printf("%s\n", ore_syntax_kind_name((OreSyntaxKind)green_node_kind(node)));
  uint32_t n = green_node_num_children(node);
  for (uint32_t i = 0; i < n; i++) {
    GreenElement el = green_node_child(node, i);
    if (el.kind == GREEN_ELEM_NODE && el.node) {
      print_tree(el.node, depth + 1);
    } else if (el.kind == GREEN_ELEM_TOKEN && el.token) {
      OreSyntaxKind tk = (OreSyntaxKind)green_token_kind(el.token);
      if (ore_kind_is_trivia(tk)) continue; // skip whitespace/comments/virtual
      for (int d = 0; d < depth + 1; d++) fputs("  ", stdout);
      printf("'%s' [%s]\n", green_token_text(el.token),
             ore_syntax_kind_name(tk));
    }
  }
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <file.ore>\n", argv[0]); return 1; }
  uint32_t source_len = 0;
  char *source = slurp(argv[1], &source_len);

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
  GreenNode *root = parse_file_green(&tokens, source, &pool, cache, &errors);

  if (errors.count) {
    fprintf(stderr, "=== %zu parse error(s) ===\n", errors.count);
    for (size_t i = 0; i < errors.count; i++) {
      ParseError *e = (ParseError *)vec_get(&errors, i);
      fprintf(stderr, "  tok %u: %s\n", e->tok_pos, e->msg);
    }
  }
  print_tree(root, 0);
  return 0;
}
