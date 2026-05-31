#include "fs.h"

#include <stdio.h>
#include <stdlib.h>

char *fs_slurp_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long sz = ftell(f);
  if (sz < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  char *buf = (char *)malloc((size_t)sz + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  size_t n = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  if (n != (size_t)sz) {
    free(buf);
    return NULL;
  }
  buf[sz] = '\0';
  if (out_len)
    *out_len = (size_t)sz;
  return buf;
}
