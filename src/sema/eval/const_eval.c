#include "const_eval.h"
#include "../../common/arena.h"
#include "../../common/hashmap.h"
#include "../../parser/ast.h"
#include "../sema.h"
#include "../sema_internal.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// parse into double. strip underscore. support scientific notation
static bool parse_float_literal(const char *text, double *out) {
  if (!text)
    return false;

  char buf[64];
  size_t j = 0;
  for (size_t i = 0; text[i] && j + 1 < sizeof(buf); i++) {
    if (text[i] != '_')
      buf[j++] = text[i];
  }
  buf[j] = '\0';
  char *end;
  *out = (float)strtod(buf, &end);
  return true;
}

// parse into a host int64_t. underscores striped. handle binary, octal, hex
static bool parse_int_literal(const char *text, int64_t *out) {
  if (!text)
    return false;

  // Strip underscores into a scratch buffer.
  char buf[64];
  size_t j = 0;
  for (size_t i = 0; text[i] && j + 1 < sizeof(buf); i++) {
    if (text[i] != '_')
      buf[j++] = text[i];
  }
  buf[j] = '\0';

  // Detect base from prefix.
  const char *digits = buf;
  int base = 10;
  if (buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X')) {
    base = 16;
    digits = buf + 2;
  } else if (buf[0] == '0' && (buf[1] == 'b' || buf[1] == 'B')) {
    base = 2;
    digits = buf + 2;
  } else if (buf[0] == '0' && (buf[1] == 'o' || buf[1] == 'O')) {
    base = 8;
    digits = buf + 2;
  }

  errno = 0;
  char *end;
  long long v = strtoll(digits, &end, base);
  if (errno || *end != '\0' || end == digits)
    return false;
  *out = (int64_t)v;
  return true;
}

struct ConstValue query_const_eval(struct Sema *s, struct Expr *expr) {
  struct ConstValue none = {.kind = CONST_NONE};
  if (!s || !expr)
    return none;

  // Cache check: have we computed this already?
  void *cached = hashmap_get(&s->const_eval_cache, (uint64_t)(uintptr_t)expr);
  if (cached) {
    return *(struct ConstValue *)cached;
  }

  // Compute.
  struct ConstValue result = none;
  switch (expr->kind) {
  case expr_Lit: {
    if (expr->lit.kind == lit_Int) {
      const char *text = pool_get(s->pool, expr->lit.string_id, 0);
      int64_t v;
      if (parse_int_literal(text, &v)) {
        result.kind = CONST_INT;
        result.int_val = v;
      }
    }
    if (expr->lit.kind == lit_Float) {
      const char *text = pool_get(s->pool, expr->lit.string_id, 0);
      double v;
      if (parse_float_literal(text, &v)) {
        result.kind = CONST_FLOAT;
        result.float_val = v;
      }
    }
    break;
  }
  default:
    break;
  }

  // Cache the result. Allocate in the arena so it lives as long as Sema.
  struct ConstValue *stored = arena_alloc(s->arena, sizeof(*stored));
  *stored = result;
  hashmap_put(&s->const_eval_cache, (uint64_t)(uintptr_t)expr, stored);

  return result;
}