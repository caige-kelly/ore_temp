#include "marshal.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Strip trailing CR/LF from a header line so we can compare the
// value portion without worrying about line-ending variants.
static void rstrip_eol(char *s, size_t *len) {
  while (*len > 0 && (s[*len - 1] == '\r' || s[*len - 1] == '\n'))
    s[--(*len)] = '\0';
}

// Read up to and including the next '\n'. Returns the line length
// (including the terminator). Returns 0 on EOF before any byte and
// SIZE_MAX on read error or buffer overflow.
//
// We use a fixed 256-byte cap on header lines. LSP headers are
// short by spec (`Content-Length:` + a decimal number); anything
// longer is malformed or hostile.
#define LSP_HEADER_LINE_MAX 256
static size_t read_header_line(FILE *in, char *buf) {
  size_t n = 0;
  for (;;) {
    int c = fgetc(in);
    if (c == EOF)
      return n; // 0 means clean EOF; non-zero means partial line
    if (n >= LSP_HEADER_LINE_MAX - 1)
      return SIZE_MAX;
    buf[n++] = (char)c;
    if (c == '\n')
      break;
  }
  buf[n] = '\0';
  return n;
}

char *lsp_read_message(FILE *in, size_t *out_len, bool *out_eof) {
  if (out_eof)
    *out_eof = false;

  // Header phase: scan `Header: value` lines until we hit an empty
  // line (\r\n alone). Capture Content-Length; ignore everything
  // else.
  size_t content_length = 0;
  bool have_length = false;
  char line[LSP_HEADER_LINE_MAX];
  bool got_any_header = false;

  for (;;) {
    size_t n = read_header_line(in, line);
    if (n == 0) {
      // Clean EOF before any header byte → caller's expected
      // end-of-stream signal.
      if (!got_any_header) {
        if (out_eof)
          *out_eof = true;
        return NULL;
      }
      fprintf(stderr, "lsp: unexpected EOF in header\n");
      return NULL;
    }
    if (n == SIZE_MAX) {
      fprintf(stderr, "lsp: header line too long\n");
      return NULL;
    }
    got_any_header = true;
    rstrip_eol(line, &n);
    if (n == 0)
      break; // empty line terminates header block

    char *colon = strchr(line, ':');
    if (!colon) {
      fprintf(stderr, "lsp: malformed header line: %s\n", line);
      return NULL;
    }
    *colon = '\0';
    char *value = colon + 1;
    while (*value == ' ' || *value == '\t')
      value++;

    if (strcasecmp(line, "Content-Length") == 0) {
      char *end = NULL;
      unsigned long v = strtoul(value, &end, 10);
      if (end == value || *end != '\0') {
        fprintf(stderr, "lsp: invalid Content-Length: %s\n", value);
        return NULL;
      }
      content_length = (size_t)v;
      have_length = true;
    }
    // Content-Type and any other header → ignored.
  }

  if (!have_length) {
    fprintf(stderr, "lsp: missing Content-Length header\n");
    return NULL;
  }

  // Cap message size at 64 MiB. LSP messages are typically a few
  // KiB; tens of MiB would be a file-open of a giant document.
  // 64 MiB gives generous headroom without inviting OOMs from a
  // malformed length.
  if (content_length > (size_t)64 * 1024 * 1024) {
    fprintf(stderr, "lsp: Content-Length too large: %zu\n", content_length);
    return NULL;
  }

  char *buf = (char *)malloc(content_length + 1);
  if (!buf) {
    fprintf(stderr, "lsp: out of memory reading body (%zu bytes)\n",
            content_length);
    return NULL;
  }
  size_t got = fread(buf, 1, content_length, in);
  if (got != content_length) {
    fprintf(stderr, "lsp: short read on body (%zu of %zu)\n", got,
            content_length);
    free(buf);
    return NULL;
  }
  buf[content_length] = '\0';
  if (out_len)
    *out_len = content_length;
  return buf;
}

bool lsp_write_message(FILE *out, const char *body, size_t len) {
  if (fprintf(out, "Content-Length: %zu\r\n\r\n", len) < 0)
    return false;
  if (fwrite(body, 1, len, out) != len)
    return false;
  if (fflush(out) != 0)
    return false;
  return true;
}
