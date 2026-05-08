#include "literals.h"

// parse into double. strip underscore. support scientific notation
bool parse_float_literal(const char *text, double *out) {
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
    *out = strtod(buf, &end);
    return true;
  }
  
  // parse into a host int64_t. underscores striped. handle binary, octal, hex
 bool parse_int_literal(const char *text, int64_t *out) {
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