#include "./lexer.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../common/stringpool.h"
#include "../diag/diag.h"
#include "./token.h"

// =====================================================================
// Lexer construction
// =====================================================================

struct Lexer lexer_new(const char *source, int file_id, struct DiagBag *diags) {
  struct Lexer l = {
      .source = source,
      .source_len = source ? strlen(source) : 0,
      .start = 0,
      .current = 0,
      .line = 1,
      .column = 1,
      .start_line = 1,
      .start_column = 1,
      .file_id = file_id,
      .diags = diags,
  };
  // Skip a leading UTF-8 BOM. The BOM is invisible to the user, so don't
  // shift the column counter past it.
  if (l.source_len >= 3 && (unsigned char)source[0] == 0xEF &&
      (unsigned char)source[1] == 0xBB && (unsigned char)source[2] == 0xBF) {
    l.current = 3;
  }
  return l;
}

// =====================================================================
// Layer A — cursor primitives
// =====================================================================

static inline char peek(const struct Lexer *l, size_t off) {
  size_t p = l->current + off;
  if (p >= l->source_len) return '\0';
  return l->source[p];
}

static inline char curr(const struct Lexer *l) { return peek(l, 0); }

static inline void advance(struct Lexer *l) {
  l->current++;
  l->column++;
}

static inline bool match(struct Lexer *l, char expected) {
  if (curr(l) != expected) return false;
  advance(l);
  return true;
}

static inline bool is_id_start(char c) {
  return isalpha((unsigned char)c) || c == '_';
}

static inline bool is_id_cont(char c) {
  return isalnum((unsigned char)c) || c == '_';
}

static struct Span span_current(const struct Lexer *l) {
  return span_new(l->file_id, (int)l->start, (int)l->current,
                  (int)l->start_column, (int)l->column, (int)l->start_line,
                  (int)l->line);
}

static void lexer_error(struct Lexer *l, const char *fmt, ...) {
  if (!l->diags) return;
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  diag_error(l->diags, span_current(l), "%s", buf);
}

// =====================================================================
// Token construction
// =====================================================================

static struct Token make_token(struct Lexer *l, StringPool *pool,
                               enum TokenKind kind) {
  struct Span span = span_current(l);
  size_t len = l->current - l->start;

  // String/byte/asm literals strip surrounding delimiters from the
  // interned text. Span still covers the delimiters for diagnostics.
  if (kind == StringLit || kind == ByteLit) {
    size_t inner_len = (len >= 2) ? len - 2 : 0;
    const char *inner = (len >= 2) ? &l->source[l->start + 1] : "";
    struct Token t = {
        .kind = kind,
        .string_id = pool_intern(pool, inner, inner_len),
        .string_len = inner_len,
        .span = span,
        .origin = Source,
    };
    return t;
  }
  if (kind == AsmLit) {
    size_t inner_len = (len >= 6) ? len - 6 : 0;
    const char *inner = (len >= 6) ? &l->source[l->start + 3] : "";
    struct Token t = {
        .kind = kind,
        .string_id = pool_intern(pool, inner, inner_len),
        .string_len = inner_len,
        .span = span,
        .origin = Source,
    };
    return t;
  }

  // Tokens whose lexeme isn't useful downstream carry an empty interned
  // string. NewLine/Eof/Error fall here; their spans still locate them.
  if (kind == NewLine || kind == Eof || kind == Error) {
    struct Token t = {
        .kind = kind,
        .string_id = pool_intern(pool, "", 0),
        .string_len = 0,
        .span = span,
        .origin = Source,
    };
    return t;
  }

  struct Token t = {
      .kind = kind,
      .string_id = pool_intern(pool, &l->source[l->start], len),
      .string_len = len,
      .span = span,
      .origin = Source,
  };
  return t;
}

// =====================================================================
// Operator dispatch helpers — keep the top-level switch readable.
// =====================================================================

// `lead` alone, or lead followed by `c2`.
static struct Token op2(struct Lexer *l, StringPool *pool, enum TokenKind one,
                        char c2, enum TokenKind two) {
  advance(l);
  if (match(l, c2)) return make_token(l, pool, two);
  return make_token(l, pool, one);
}

// `lead` alone, lead+a, or lead+b. First match wins, in declaration order.
static struct Token op3(struct Lexer *l, StringPool *pool, enum TokenKind one,
                        char a, enum TokenKind two_a, char b,
                        enum TokenKind two_b) {
  advance(l);
  if (match(l, a)) return make_token(l, pool, two_a);
  if (match(l, b)) return make_token(l, pool, two_b);
  return make_token(l, pool, one);
}

// `lead` alone, lead+a, lead+b, or lead+c. Used by `<` (=, <, -).
static struct Token op4(struct Lexer *l, StringPool *pool, enum TokenKind one,
                        char a, enum TokenKind two_a, char b,
                        enum TokenKind two_b, char c, enum TokenKind two_c) {
  advance(l);
  if (match(l, a)) return make_token(l, pool, two_a);
  if (match(l, b)) return make_token(l, pool, two_b);
  if (match(l, c)) return make_token(l, pool, two_c);
  return make_token(l, pool, one);
}

// =====================================================================
// Layer B — sub-lexers
// =====================================================================

// Linear scan over a small table — perf is deferred per the plan; the
// strcmp chain it replaced was already linear.
static enum TokenKind keyword_kind(const char *s, size_t len) {
  static const struct {
    const char *kw;
    size_t kw_len;
    enum TokenKind kind;
  } table[] = {
      {"if", 2, If},
      {"elif", 4, Elif},
      {"else", 4, Else},
      {"true", 4, True},
      {"false", 5, False},
      {"nil", 3, Nil},
      {"void", 4, Void},
      {"const", 5, Const},
      {"type", 4, Type},
      {"orelse", 6, OrElse},
      {"struct", 6, Struct},
      {"enum", 4, Enum},
      {"union", 5, Union},
      {"effect", 6, Effect},
      {"scoped", 6, Scoped},
      {"linear", 6, Linear},
      {"final", 5, Final},
      {"val", 3, Val},
      {"named", 5, Named},
      {"handler", 7, Handler},
      {"handle", 6, Handle},
      {"resume", 6, Resume},
      {"override", 8, Override},
      {"mask", 4, Mask},
      {"with", 4, With},
      {"comptime", 8, Comptime},
      {"pub", 3, Pub},
      {"noreturn", 8, NoReturn},
      {"switch", 6, Switch},
      {"continue", 8, Continue},
      {"break", 5, Break},
      {"finally", 7, Finally},
      {"initially", 9, Initially},
      {"anytype", 7, AnyType},
      {"in", 2, In},
      {"return", 6, Return},
      {"fn", 2, Fn},
      {"loop", 4, Loop},
      {"defer", 5, Defer},
      {"ctl", 3, Ctl},
  };
  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    if (table[i].kw_len == len && memcmp(table[i].kw, s, len) == 0) {
      return table[i].kind;
    }
  }
  return Identifier;
}

static struct Token lex_identifier_or_keyword(struct Lexer *l,
                                              StringPool *pool) {
  while (is_id_cont(curr(l))) advance(l);
  size_t len = l->current - l->start;
  // Bare `_` is a wildcard, not an identifier.
  if (len == 1 && l->source[l->start] == '_') {
    return make_token(l, pool, Underscore);
  }
  enum TokenKind kind = keyword_kind(&l->source[l->start], len);
  return make_token(l, pool, kind);
}

static void scan_decimal_digits(struct Lexer *l) {
  while (isdigit((unsigned char)curr(l)) || curr(l) == '_') advance(l);
}

static struct Token lex_number(struct Lexer *l, StringPool *pool) {
  char c0 = curr(l);
  char c1 = peek(l, 1);

  // Alternate bases: 0x, 0X, 0b, 0B, 0o, 0O.
  if (c0 == '0' && (c1 == 'x' || c1 == 'X' || c1 == 'b' || c1 == 'B' ||
                    c1 == 'o' || c1 == 'O')) {
    advance(l);  // '0'
    advance(l);  // base prefix
    if (c1 == 'x' || c1 == 'X') {
      while (isxdigit((unsigned char)curr(l)) || curr(l) == '_') advance(l);
    } else if (c1 == 'b' || c1 == 'B') {
      while (curr(l) == '0' || curr(l) == '1' || curr(l) == '_') advance(l);
    } else {  // octal
      while ((curr(l) >= '0' && curr(l) <= '7') || curr(l) == '_') advance(l);
    }
    return make_token(l, pool, IntLit);
  }

  scan_decimal_digits(l);

  bool is_float = false;

  // Fractional part: only if the dot is followed by a digit, so `1..2`
  // (range) and `1.foo` (postfix) still parse correctly.
  if (curr(l) == '.' && isdigit((unsigned char)peek(l, 1))) {
    is_float = true;
    advance(l);  // '.'
    scan_decimal_digits(l);
  }

  // Exponent: e/E [+/-] digits — must have at least one digit after.
  if (curr(l) == 'e' || curr(l) == 'E') {
    size_t look = 1;
    if (peek(l, look) == '+' || peek(l, look) == '-') look++;
    if (isdigit((unsigned char)peek(l, look))) {
      is_float = true;
      advance(l);  // e/E
      if (curr(l) == '+' || curr(l) == '-') advance(l);
      scan_decimal_digits(l);
    }
  }

  return make_token(l, pool, is_float ? FloatLit : IntLit);
}

// Body of a `"…"` or `'…'` literal. Returns true if the closing quote was
// consumed cleanly; false if EOF/newline ended the literal early (already
// diagnosed). On false the caller emits an Error token at the partial span.
static bool scan_quoted(struct Lexer *l, char quote, const char *what) {
  advance(l);  // opening quote
  while (curr(l) != quote) {
    char c = curr(l);
    if (c == '\0' || c == '\n' || c == '\r') {
      lexer_error(l, "unterminated %s literal", what);
      return false;
    }
    if (c == '\\') {
      advance(l);  // backslash
      char esc = curr(l);
      switch (esc) {
      case '\\':
      case '"':
      case '\'':
      case 'n':
      case 't':
      case 'r':
      case '0':
        advance(l);
        break;
      case 'x': {
        // \xNN — exactly two hex digits
        advance(l);  // consume 'x'
        for (int i = 0; i < 2; i++) {
          if (!isxdigit((unsigned char)curr(l))) {
            lexer_error(l,
                        "'\\x' escape requires exactly two hex digits");
            // Don't advance — let the outer loop continue from here.
            goto escape_done;
          }
          advance(l);
        }
        break;
      }
      case '\0':
        lexer_error(l, "unterminated %s literal", what);
        return false;
      default:
        lexer_error(l, "unknown escape sequence '\\%c'", esc);
        advance(l);  // best-effort recovery
        break;
      }
    escape_done:
      continue;
    }
    advance(l);
  }
  advance(l);  // closing quote
  return true;
}

static struct Token lex_string(struct Lexer *l, StringPool *pool) {
  if (!scan_quoted(l, '"', "string")) return make_token(l, pool, Error);
  return make_token(l, pool, StringLit);
}

static struct Token lex_byte(struct Lexer *l, StringPool *pool) {
  if (!scan_quoted(l, '\'', "byte")) return make_token(l, pool, Error);
  return make_token(l, pool, ByteLit);
}

// `// …` to end-of-line. The newline itself is left for lex_newline so the
// layout pass sees a clean Comment-then-NewLine sequence.
static struct Token lex_line_comment(struct Lexer *l, StringPool *pool) {
  advance(l);  // '/'
  advance(l);  // '/'
  while (curr(l) != '\0' && curr(l) != '\n' && curr(l) != '\r') advance(l);
  return make_token(l, pool, Comment);
}

// `/* … */`. Nests Rust-style: inner `/* … */` pairs increment a depth
// counter so you can comment out code that already contains block comments.
static struct Token lex_block_comment(struct Lexer *l, StringPool *pool) {
  advance(l);  // '/'
  advance(l);  // '*'
  int depth = 1;
  while (curr(l) != '\0') {
    char c = curr(l);
    if (c == '/' && peek(l, 1) == '*') {
      advance(l);
      advance(l);
      depth++;
      continue;
    }
    if (c == '*' && peek(l, 1) == '/') {
      advance(l);
      advance(l);
      depth--;
      if (depth == 0) return make_token(l, pool, Comment);
      continue;
    }
    if (c == '\n') {
      l->current++;
      l->line++;
      l->column = 1;
      continue;
    }
    if (c == '\r') {
      // Treat \r and \r\n as one logical newline, matching lex_newline.
      l->current++;
      if (curr(l) == '\n') l->current++;
      l->line++;
      l->column = 1;
      continue;
    }
    advance(l);
  }
  lexer_error(l, "unterminated block comment");
  return make_token(l, pool, Error);
}

// ``` … ``` — inline assembly. Body is verbatim (no escape processing);
// span covers all six backticks; interned string is the inner text only.
static struct Token lex_asm_lit(struct Lexer *l, StringPool *pool) {
  advance(l); advance(l); advance(l);  // opening ```
  while (curr(l) != '\0') {
    if (curr(l) == '`' && peek(l, 1) == '`' && peek(l, 2) == '`') {
      advance(l); advance(l); advance(l);
      return make_token(l, pool, AsmLit);
    }
    if (curr(l) == '\n') {
      l->current++;
      l->line++;
      l->column = 1;
      continue;
    }
    if (curr(l) == '\r') {
      l->current++;
      if (curr(l) == '\n') l->current++;
      l->line++;
      l->column = 1;
      continue;
    }
    advance(l);
  }
  lexer_error(l, "unterminated inline asm block (expected closing ```)");
  return make_token(l, pool, Error);
}

static struct Token lex_spaces(struct Lexer *l, StringPool *pool) {
  while (curr(l) == ' ') advance(l);
  return make_token(l, pool, Space);
}

// Handles \n, \r\n, and lone \r as a single NewLine token. The line
// counter bumps after the token is built so the span describes the line
// that ended, not the next one.
static struct Token lex_newline(struct Lexer *l, StringPool *pool) {
  if (curr(l) == '\r') {
    advance(l);
    if (curr(l) == '\n') advance(l);
  } else /* '\n' */ {
    advance(l);
  }
  struct Token t = make_token(l, pool, NewLine);
  l->line++;
  l->column = 1;
  return t;
}

// =====================================================================
// Layer C — top-level dispatch
// =====================================================================

struct Token tokenizer(struct Lexer *l, StringPool *pool) {
  l->start = l->current;
  l->start_line = l->line;
  l->start_column = l->column;

  if (l->current >= l->source_len) {
    return make_token(l, pool, Eof);
  }

  char c = curr(l);

  if (c == '\0') return make_token(l, pool, Eof);
  if (isdigit((unsigned char)c)) return lex_number(l, pool);
  if (is_id_start(c)) return lex_identifier_or_keyword(l, pool);

  switch (c) {
  case ' ':
    return lex_spaces(l, pool);
  case '\n':
  case '\r':
    return lex_newline(l, pool);
  case '\t':
    lexer_error(l,
                "tab characters are not allowed; use spaces for indentation");
    advance(l);
    return make_token(l, pool, Error);

  case '"':
    return lex_string(l, pool);
  case '\'':
    return lex_byte(l, pool);
  case '`':
    if (peek(l, 1) == '`' && peek(l, 2) == '`') return lex_asm_lit(l, pool);
    lexer_error(l,
                "single backtick is not a valid token; use ``` for inline asm");
    advance(l);
    return make_token(l, pool, Error);

  // Single-char delimiters and sigils.
  case '(': advance(l); return make_token(l, pool, LParen);
  case ')': advance(l); return make_token(l, pool, RParen);
  case '[': advance(l); return make_token(l, pool, LBracket);
  case ']': advance(l); return make_token(l, pool, RBracket);
  case '{': advance(l); return make_token(l, pool, LBrace);
  case '}': advance(l); return make_token(l, pool, RBrace);
  case ';': advance(l); return make_token(l, pool, Semicolon);
  case ',': advance(l); return make_token(l, pool, Comma);
  case '@': advance(l); return make_token(l, pool, At);
  case '#': advance(l); return make_token(l, pool, Hash);
  case '~': advance(l); return make_token(l, pool, Tilde);
  case '^': advance(l); return make_token(l, pool, Caret);
  case '?': advance(l); return make_token(l, pool, Question);

  // Operators with optional second char.
  case '+': return op3(l, pool, Plus,    '=', PlusEqual,  '+', PlusPlus);
  case '-': return op3(l, pool, Minus,   '>', RightArrow, '=', MinusEqual);
  case '*': return op3(l, pool, Star,    '=', StarEqual,  '*', StarStar);
  case '%': return op2(l, pool, Percent, '=', PercentEqual);
  case '&': return op2(l, pool, Ampersand, '&', AmpersandAmpersand);
  case '|': return op2(l, pool, Pipe,    '|', PipePipe);
  case '!': return op2(l, pool, Bang,    '=', BangEqual);
  case '=': return op3(l, pool, Equal,   '=', EqualEqual, '>', FatArrow);
  case '<':
    return op4(l, pool, Less, '=', LessEqual, '<', ShiftLeft, '-', LeftArrow);
  case '>': return op3(l, pool, Greater, '=', GreaterEqual, '>', ShiftRight);
  case ':': return op3(l, pool, Colon,   ':', ColonColon, '=', ColonEqual);

  case '/':
    if (peek(l, 1) == '/') return lex_line_comment(l, pool);
    if (peek(l, 1) == '*') return lex_block_comment(l, pool);
    return op2(l, pool, ForwardSlash, '=', ForwardSlashEqual);

  case '.': {
    advance(l);  // first '.'
    if (!match(l, '.')) return make_token(l, pool, Dot);
    if (match(l, '.')) return make_token(l, pool, DotDotDot);
    return make_token(l, pool, DotDot);
  }
  }

  // Unrecognized character: diagnose, advance one byte, emit Error so the
  // parser can resync.
  lexer_error(l, "unexpected character '%c'", (unsigned char)c);
  advance(l);
  return make_token(l, pool, Error);
}
