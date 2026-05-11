#include "parse_source.h"

#include "../diag/diag.h"
#include "../lexer/layout.h"
#include "../lexer/lexer.h"
#include "../lexer/token.h"
#include "parser.h"

Vec *parse_source(Arena *ast_arena, Arena *scratch_arena, StringPool *pool,
                  struct DiagBag *diags, int file_id, const char *source,
                  size_t source_len) {
  (void)source_len;
  if (!ast_arena || !scratch_arena || !pool || !source)
    return NULL;

  struct Lexer lexer = lexer_new(source, file_id, diags);
  Vec tokens;
  vec_init_in(&tokens, scratch_arena, sizeof(struct Token));

  struct Token token;
  for (;;) {
    token = tokenizer(&lexer, pool);
    vec_push(&tokens, &token);
    if (token.kind == Eof)
      break;
  }

  Vec *laid_out = normalizer_in(&tokens, pool, scratch_arena, diags);
  if (!laid_out)
    return NULL;

  struct Parser parser =
      parser_new_in_with_diags(laid_out, pool, ast_arena, diags, file_id);
  return parse(&parser);
}
