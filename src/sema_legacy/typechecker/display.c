#include "display.h"

#include <stdio.h>
#include <string.h>

#include "db/storage/stringpool.h"
#include "db/ids/ids.h"
#include "../scope/scope.h"
#include "../sema.h"
#include "type.h"

// Bounded append helper. Writes `src` into `buf[*pos..buflen]`,
// advancing *pos. Truncates safely on overflow.
static void append(char *buf, size_t buflen, size_t *pos, const char *src) {
  if (!src || !buf || !pos || *pos >= buflen)
    return;
  size_t remaining = buflen - *pos - 1; // leave room for NUL
  size_t src_len = strlen(src);
  size_t to_copy = src_len < remaining ? src_len : remaining;
  memcpy(buf + *pos, src, to_copy);
  *pos += to_copy;
  buf[*pos] = '\0';
}

static void render(struct Sema *s, const struct Type *t, char *buf,
                   size_t buflen, size_t *pos) {
  if (!t) {
    append(buf, buflen, pos, "<null>");
    return;
  }

  switch (t->kind) {
  case TY_FN: {
    append(buf, buflen, pos, "fn(");
    for (size_t i = 0; i < t->fn.param_count; i++) {
      if (i > 0)
        append(buf, buflen, pos, ", ");
      render(s, t->fn.params[i], buf, buflen, pos);
    }
    append(buf, buflen, pos, ") -> ");
    render(s, t->fn.ret, buf, buflen, pos);
    return;
  }
  case TY_PTR:
    append(buf, buflen, pos, "^");
    if (t->ptr.is_const)
      append(buf, buflen, pos, "const ");
    render(s, t->ptr.elem, buf, buflen, pos);
    return;
  case TY_MANY_PTR:
    append(buf, buflen, pos, "[^]");
    if (t->many_ptr.is_const)
      append(buf, buflen, pos, "const ");
    render(s, t->many_ptr.elem, buf, buflen, pos);
    return;
  case TY_SLICE:
    append(buf, buflen, pos, "[]");
    if (t->slice.is_const)
      append(buf, buflen, pos, "const ");
    render(s, t->slice.elem, buf, buflen, pos);
    return;
  case TY_ARRAY: {
    char sizebuf[32];
    snprintf(sizebuf, sizeof(sizebuf), "[%llu]",
             (unsigned long long)t->array.size);
    append(buf, buflen, pos, sizebuf);
    render(s, t->array.elem, buf, buflen, pos);
    return;
  }
  case TY_OPTIONAL:
    append(buf, buflen, pos, "?");
    render(s, t->optional.elem, buf, buflen, pos);
    return;
  case TY_STRUCT: {
    if (s) {
      struct DefInfo *di = def_info(s, t->struct_.def);
      const char *name = di ? pool_get(&s->pool, di->name_id, 0) : NULL;
      append(buf, buflen, pos, name ? name : "<anon-struct>");
    } else {
      append(buf, buflen, pos, "struct");
    }
    return;
  }
  case TY_ENUM: {
    if (s) {
      struct DefInfo *di = def_info(s, t->enum_.def);
      const char *name = di ? pool_get(&s->pool, di->name_id, 0) : NULL;
      append(buf, buflen, pos, name ? name : "<anon-enum>");
    } else {
      append(buf, buflen, pos, "enum");
    }
    return;
  }
  default:
    // Primitives — fall through to type_name.
    append(buf, buflen, pos, type_name(t));
    return;
  }
}

const char *type_to_string(struct Sema *s, const struct Type *t, char *buf,
                           size_t buflen) {
  if (!buf || buflen == 0)
    return "";
  buf[0] = '\0';
  size_t pos = 0;
  render(s, t, buf, buflen, &pos);
  return buf;
}
