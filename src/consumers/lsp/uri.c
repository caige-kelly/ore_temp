#include "uri.h"

#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifdef __linux__
#include <linux/limits.h>
#endif

static int hex_value(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F')
    return 10 + (c - 'A');
  return -1;
}

// Percent-decode `src` into a freshly malloc'd null-terminated
// string. Returns NULL on encoding error (truncated `%X` or
// invalid hex).
static char *percent_decode(const char *src) {
  size_t in_len = strlen(src);
  char *out = (char *)malloc(in_len + 1);
  if (!out)
    return NULL;
  size_t j = 0;
  for (size_t i = 0; i < in_len;) {
    char c = src[i];
    if (c == '%') {
      if (i + 2 >= in_len) {
        free(out);
        return NULL;
      }
      int hi = hex_value(src[i + 1]);
      int lo = hex_value(src[i + 2]);
      if (hi < 0 || lo < 0) {
        free(out);
        return NULL;
      }
      out[j++] = (char)((hi << 4) | lo);
      i += 3;
    } else {
      out[j++] = c;
      i++;
    }
  }
  out[j] = '\0';
  return out;
}

char *lsp_uri_to_path(const char *uri) {
  if (!uri)
    return NULL;
  // Only `file://` accepted today. Any other scheme returns NULL
  // so the caller can log and drop the event.
  static const char prefix[] = "file://";
  size_t prefix_len = sizeof(prefix) - 1;
  if (strncmp(uri, prefix, prefix_len) != 0)
    return NULL;
  const char *path_part = uri + prefix_len;

  // `file://host/path` is allowed by RFC 8089 but in practice LSP
  // clients always emit `file:///path` (empty authority). Skip
  // anything up to the first `/` to be tolerant.
  const char *first_slash = strchr(path_part, '/');
  if (!first_slash)
    return NULL;
  path_part = first_slash; // includes the leading slash

  char *decoded = percent_decode(path_part);
  if (!decoded)
    return NULL;

  // Windows: `file:///C:/foo` decodes to `/C:/foo`; strip the
  // leading slash. POSIX paths start with `/` and we leave them
  // alone. Detect Windows by `:` at decoded[2].
  if (decoded[0] == '/' && decoded[1] && decoded[2] == ':') {
    memmove(decoded, decoded + 1, strlen(decoded));
  }

  // Canonicalize via realpath when the file actually exists. When
  // it doesn't (fresh untitled→saveAs case before the first save
  // round-trip), return the lexically decoded path so the LSP can
  // still track an identity for it.
  char resolved[PATH_MAX];
  if (realpath(decoded, resolved)) {
    free(decoded);
    return strdup(resolved);
  }
  return decoded;
}

// L4 — per RFC 3986, the path component of a file URI may contain only
// `pchar / "/"`: unreserved (ALPHA / DIGIT / "-" / "." / "_" / "~"),
// sub-delims (`!$&'()*+,;=`), and `:@`, plus `/` as path separator.
// Everything else (space, `#`, `?`, `%`, ≥0x80, controls) must be
// percent-encoded. Strict LSP clients (newer VSCode, Helix, some
// Neovim plugins) reject malformed URIs silently.
static bool uri_path_char_safe(unsigned char c) {
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
      (c >= '0' && c <= '9'))
    return true;
  switch (c) {
  // unreserved (minus alnum)
  case '-': case '.': case '_': case '~':
  // sub-delims
  case '!': case '$': case '&': case '\'': case '(': case ')':
  case '*': case '+': case ',': case ';': case '=':
  // pchar extras + path separator
  case ':': case '@': case '/':
    return true;
  default:
    return false;
  }
}

char *lsp_path_to_uri(const char *path) {
  if (!path)
    return NULL;
  static const char prefix[] = "file://";
  size_t plen = strlen(path);
  size_t pre_len = sizeof(prefix) - 1;
  // Worst case: every byte becomes %XX (3 bytes).
  char *out = (char *)malloc(pre_len + 3 * plen + 1);
  if (!out)
    return NULL;
  memcpy(out, prefix, pre_len);
  size_t w = pre_len;
  static const char hex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < plen; i++) {
    unsigned char c = (unsigned char)path[i];
    if (uri_path_char_safe(c)) {
      out[w++] = (char)c;
    } else {
      out[w++] = '%';
      out[w++] = hex[c >> 4];
      out[w++] = hex[c & 0xF];
    }
  }
  out[w] = '\0';
  return out;
}
