#include "path.h"

#include <string.h>

size_t path_normalize(const char *dir, size_t dir_len, const char *rel,
                      size_t rel_len, char *out, size_t out_cap) {
  if (out_cap < 2)
    return 0;

  // Build the initial absolute buffer: dir + '/' + rel  (or just rel if
  // rel is absolute).
  char buf[ORE_PATH_MAX];
  size_t bn = 0;
  if (rel_len > 0 && rel[0] == '/') {
    if (rel_len + 1 > sizeof(buf))
      return 0;
    memcpy(buf, rel, rel_len);
    bn = rel_len;
  } else {
    if (dir_len + 1 + rel_len + 1 > sizeof(buf))
      return 0;
    memcpy(buf, dir, dir_len);
    bn = dir_len;
    if (bn == 0 || buf[bn - 1] != '/') {
      buf[bn++] = '/';
    }
    memcpy(buf + bn, rel, rel_len);
    bn += rel_len;
  }
  buf[bn] = '\0';

  // Walk components, building out by pushing components and popping on
  // '..'. The output retains the leading '/' if buf was absolute.
  size_t out_n = 0;
  if (buf[0] == '/') {
    out[out_n++] = '/';
  }

  size_t i = 0;
  while (i < bn) {
    while (i < bn && buf[i] == '/')
      i++;
    if (i >= bn)
      break;
    size_t start = i;
    while (i < bn && buf[i] != '/')
      i++;
    size_t comp_len = i - start;
    if (comp_len == 0)
      continue;
    if (comp_len == 1 && buf[start] == '.')
      continue; // skip "."
    if (comp_len == 2 && buf[start] == '.' && buf[start + 1] == '.') {
      // Pop the previous component (if any beyond the leading '/').
      if (out_n > 1) {
        if (out[out_n - 1] == '/')
          out_n--;
        while (out_n > 0 && out[out_n - 1] != '/')
          out_n--;
      }
      continue;
    }
    if (out_n > 0 && out[out_n - 1] != '/') {
      if (out_n + 1 >= out_cap)
        return 0;
      out[out_n++] = '/';
    }
    if (out_n + comp_len >= out_cap)
      return 0;
    memcpy(out + out_n, buf + start, comp_len);
    out_n += comp_len;
  }

  while (out_n > 1 && out[out_n - 1] == '/')
    out_n--;
  if (out_n < out_cap)
    out[out_n] = '\0';
  else
    out[out_cap - 1] = '\0';
  return out_n;
}

size_t path_dirname_len(const char *path, size_t path_len) {
  for (size_t i = path_len; i > 0; i--) {
    if (path[i - 1] == '/')
      return i - 1;
  }
  return 0;
}
