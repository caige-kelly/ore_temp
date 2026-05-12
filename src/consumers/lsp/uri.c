#include "uri.h"

#include <ctype.h>
#include <limits.h>
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
