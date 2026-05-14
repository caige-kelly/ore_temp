#include "./lexer.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../db/storage/stringpool.h"
#include "../db/storage/vec.h"
#include "./token.h"

// =====================================================================
// Internal lex state. Stack-local in lex(); never escapes the call.
// =====================================================================

typedef struct {
  const char *source;
  uint32_t source_len;
  uint32_t pos;       // current byte offset
  uint32_t tok_start; // byte offset where current token began

  StringPool *pool;
  Vec *tokens;      // Vec<Token>     — arena-backed, caller-init'd
  Vec *line_starts; // Vec<uint32_t>  — arena-backed, caller-init'd
} Lex;

// =====================================================================
// Cursor primitives
// =====================================================================

static inline char peek_at(const Lex *l, uint32_t off) {
  uint32_t p = l->pos + off;
  if (p >= l->source_len)
    return '\0';
  return l->source[p];
}

static inline char curr(const Lex *l) { return peek_at(l, 0); }

static inline void advance(Lex *l) {
  if (l->pos < l->source_len)
    l->pos++;
}

static inline bool match(Lex *l, char c) {
  if (curr(l) != c)
    return false;
  advance(l);
  return true;
}

static inline bool is_id_start(char c) {
  unsigned char u = (unsigned char)c;
  return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u == '_';
}

static inline bool is_id_cont(char c) {
  unsigned char u = (unsigned char)c;
  return is_id_start(c) || (u >= '0' && u <= '9');
}

// =====================================================================
// Token emission
// =====================================================================

static void emit(Lex *l, TokenKind kind, StrId sid) {
  Token t = {
      .kind = kind,
      ._pad0 = 0,
      .string_id = sid,
      .start = l->tok_start,
      .byte_end = l->pos,
  };
  vec_push(l->tokens, &t);
}

// Push a token with its lexeme interned. Used for tokens whose textual
// content is needed downstream (identifiers, numeric literals).
static void emit_interned(Lex *l, TokenKind kind) {
  uint32_t len = l->pos - l->tok_start;
  StrId sid = pool_intern(l->pool, &l->source[l->tok_start], len);
  emit(l, kind, sid);
}

// Push a token with no interned text. For operators, delimiters, and
// reserved keywords whose kind alone identifies them.
static void emit_plain(Lex *l, TokenKind kind) { emit(l, kind, STR_ID_NONE); }

// =====================================================================
// Line-start recording. Call AFTER consuming a newline (or CRLF pair).
// =====================================================================

static void record_line_start(Lex *l) {
  uint32_t pos = l->pos;
  vec_push(l->line_starts, &pos);
}

// =====================================================================
// Diagnostics
// =====================================================================

// Stub. The diag subsystem is being rewritten; lex_error is the
// single hook point. Lexer always emits a TK_ERROR token regardless,
// so the parser can recover and continue.
//
// TODO(diag): wire up against the new diag subsystem. The intended
// shape:
//   diag_emit(l->diags, l->tok_start, l->pos, DIAG_ERROR, msg);
static void lex_error(Lex *l, const char *msg) {
  (void)l;
  (void)msg;
}

// =====================================================================
// Keyword recognizer.
//
// Reserved keywords ONLY. Contextual keywords (val, final, raw, ctl,
// override, named, in, scoped, linear) are NOT in this table — they
// lex as TK_IDENTIFIER and the parser disambiguates via StrId compare
// against db.names at positions where they're meaningful.
//
// Linear scan over ~30 entries on every identifier. The cost is dwarfed
// by the pool intern that follows for non-keyword identifiers, so a
// perfect-hash is premature.
// =====================================================================

typedef struct {
  const char *kw;
  uint32_t len;
  TokenKind kind;
} KwEntry;

static const KwEntry kw_table[] = {
    // 2-char
    {"if", 2, TK_IF},
    {"fn", 2, TK_FN},
    {"Fn", 2, TK_FN_TYPE},

    // 3-char
    {"pub", 3, TK_PUB},
    {"nil", 3, TK_NIL},

    // 4-char
    {"elif", 4, TK_ELIF},
    {"else", 4, TK_ELSE},
    {"true", 4, TK_TRUE},
    {"loop", 4, TK_LOOP},
    {"void", 4, TK_VOID},
    {"type", 4, TK_TYPE},
    {"enum", 4, TK_ENUM},
    {"with", 4, TK_WITH},
    {"mask", 4, TK_MASK},

    // 5-char
    {"false", 5, TK_FALSE},
    {"const", 5, TK_CONST},
    {"break", 5, TK_BREAK},
    {"defer", 5, TK_DEFER},
    {"union", 5, TK_UNION},

    // 6-char
    {"struct", 6, TK_STRUCT},
    {"effect", 6, TK_EFFECT},
    {"return", 6, TK_RETURN},
    {"orelse", 6, TK_ORELSE},
    {"switch", 6, TK_SWITCH},
    {"handle", 6, TK_HANDLE},

    // 7-char
    {"anytype", 7, TK_ANYTYPE},
    {"handler", 7, TK_HANDLER},

    // 8-char
    {"comptime", 8, TK_COMPTIME},
    {"continue", 8, TK_CONTINUE},
    {"noreturn", 8, TK_NORETURN},
};

#define KW_COUNT (sizeof(kw_table) / sizeof(kw_table[0]))

static TokenKind keyword_kind(const char *s, uint32_t len) {
  for (size_t i = 0; i < KW_COUNT; i++) {
    if (kw_table[i].len == len && memcmp(kw_table[i].kw, s, len) == 0) {
      return kw_table[i].kind;
    }
  }
  return TK_IDENTIFIER;
}

// =====================================================================
// Sub-lexers
// =====================================================================

static void lex_identifier_or_keyword(Lex *l) {
  while (is_id_cont(curr(l)))
    advance(l);
  uint32_t len = l->pos - l->tok_start;

  // Bare `_` is the wildcard token, not an identifier.
  if (len == 1 && l->source[l->tok_start] == '_') {
    emit_plain(l, TK_UNDERSCORE);
    return;
  }

  TokenKind kind = keyword_kind(&l->source[l->tok_start], len);
  if (kind == TK_IDENTIFIER) {
    // Real identifier — intern the lexeme; sema needs the text.
    emit_interned(l, TK_IDENTIFIER);
  } else {
    // Reserved keyword — consumers dispatch on `kind` alone; skip
    // the pool insert.
    emit_plain(l, kind);
  }
}

// Numbers --------------------------------------------------------------

static void scan_decimal_digits(Lex *l) {
  while (isdigit((unsigned char)curr(l)) || curr(l) == '_') {
    advance(l);
  }
}

static void lex_number(Lex *l) {
  char c0 = curr(l);
  char c1 = peek_at(l, 1);

  // Alternate bases: 0x, 0X, 0b, 0B, 0o, 0O.
  if (c0 == '0' && (c1 == 'x' || c1 == 'X' || c1 == 'b' || c1 == 'B' ||
                    c1 == 'o' || c1 == 'O')) {
    advance(l);
    advance(l);
    if (c1 == 'x' || c1 == 'X') {
      while (isxdigit((unsigned char)curr(l)) || curr(l) == '_')
        advance(l);
    } else if (c1 == 'b' || c1 == 'B') {
      while (curr(l) == '0' || curr(l) == '1' || curr(l) == '_')
        advance(l);
    } else {
      while ((curr(l) >= '0' && curr(l) <= '7') || curr(l) == '_')
        advance(l);
    }
    emit_interned(l, TK_INT_LIT);
    return;
  }

  scan_decimal_digits(l);

  bool is_float = false;

  // Fractional part: only if the dot is followed by a digit. So
  // `1..2` (range) and `1.foo` (postfix) still parse correctly.
  if (curr(l) == '.' && isdigit((unsigned char)peek_at(l, 1))) {
    is_float = true;
    advance(l);
    scan_decimal_digits(l);
  }

  // Exponent: e/E [+/-] digits — at least one digit required.
  if (curr(l) == 'e' || curr(l) == 'E') {
    uint32_t look = 1;
    if (peek_at(l, look) == '+' || peek_at(l, look) == '-')
      look++;
    if (isdigit((unsigned char)peek_at(l, look))) {
      is_float = true;
      advance(l);
      if (curr(l) == '+' || curr(l) == '-')
        advance(l);
      scan_decimal_digits(l);
    }
  }

  emit_interned(l, is_float ? TK_FLOAT_LIT : TK_INT_LIT);
}

// Strings & byte literals ---------------------------------------------

// Scan the body of a `"…"` or `'…'` literal. Returns true on clean close,
// false when EOF/newline ended the literal early (lex_error already
// pushed).
static bool scan_quoted(Lex *l, char quote) {
  advance(l); // opening quote
  while (curr(l) != quote) {
    char c = curr(l);
    if (c == '\0' || c == '\n' || c == '\r') {
      lex_error(l, "unterminated quoted literal");
      return false;
    }
    if (c == '\\') {
      advance(l);
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
        advance(l);
        for (int i = 0; i < 2; i++) {
          if (!isxdigit((unsigned char)curr(l))) {
            lex_error(l, "'\\x' escape requires exactly two hex digits");
            goto escape_done;
          }
          advance(l);
        }
        break;
      }
      case '\0':
        lex_error(l, "unterminated quoted literal");
        return false;
      default:
        lex_error(l, "unknown escape sequence");
        advance(l);
        break;
      }
    escape_done:
      continue;
    }
    advance(l);
  }
  advance(l); // closing quote
  return true;
}

// Intern the body of a single-byte-delimited quoted literal. For `"foo"`
// interns `foo`. Empty literal interns empty string.
static StrId intern_quoted_body(Lex *l) {
  uint32_t total = l->pos - l->tok_start;
  if (total < 2)
    return pool_intern(l->pool, "", 0);
  return pool_intern(l->pool, &l->source[l->tok_start + 1], total - 2);
}

static void lex_string(Lex *l) {
  if (!scan_quoted(l, '"')) {
    emit_plain(l, TK_ERROR);
    return;
  }
  emit(l, TK_STRING_LIT, intern_quoted_body(l));
}

static void lex_byte_lit(Lex *l) {
  if (!scan_quoted(l, '\'')) {
    emit_plain(l, TK_ERROR);
    return;
  }
  emit(l, TK_BYTE_LIT, intern_quoted_body(l));
}

// Comments and inline asm ---------------------------------------------

// `// …` to end-of-line. The newline byte itself is left for
// lex_newline so the layout pass sees a clean Comment→NewLine sequence.
static void lex_line_comment(Lex *l) {
  advance(l);
  advance(l);
  while (curr(l) != '\0' && curr(l) != '\n' && curr(l) != '\r') {
    advance(l);
  }
  emit_interned(l, TK_COMMENT);
}

// `/* … */` with Rust-style nesting.
static void lex_block_comment(Lex *l) {
  advance(l);
  advance(l);
  int depth = 1;
  while (curr(l) != '\0') {
    char c = curr(l);
    if (c == '/' && peek_at(l, 1) == '*') {
      advance(l);
      advance(l);
      depth++;
      continue;
    }
    if (c == '*' && peek_at(l, 1) == '/') {
      advance(l);
      advance(l);
      depth--;
      if (depth == 0) {
        emit_interned(l, TK_COMMENT);
        return;
      }
      continue;
    }
    if (c == '\n') {
      advance(l);
      record_line_start(l);
      continue;
    }
    if (c == '\r') {
      advance(l);
      if (curr(l) == '\n')
        advance(l);
      record_line_start(l);
      continue;
    }
    advance(l);
  }
  lex_error(l, "unterminated block comment");
  emit_plain(l, TK_ERROR);
}

// ``` … ``` — inline assembly. Body verbatim (no escape processing);
// the interned string covers the inner text only, byte range covers
// the full literal including the six backticks.
static void lex_asm_lit(Lex *l) {
  advance(l);
  advance(l);
  advance(l);
  while (curr(l) != '\0') {
    if (curr(l) == '`' && peek_at(l, 1) == '`' && peek_at(l, 2) == '`') {
      advance(l);
      advance(l);
      advance(l);
      uint32_t total = l->pos - l->tok_start;
      StrId sid;
      if (total >= 6) {
        sid = pool_intern(l->pool, &l->source[l->tok_start + 3], total - 6);
      } else {
        sid = pool_intern(l->pool, "", 0);
      }
      emit(l, TK_ASM_LIT, sid);
      return;
    }
    if (curr(l) == '\n') {
      advance(l);
      record_line_start(l);
      continue;
    }
    if (curr(l) == '\r') {
      advance(l);
      if (curr(l) == '\n')
        advance(l);
      record_line_start(l);
      continue;
    }
    advance(l);
  }
  lex_error(l, "unterminated inline asm block (expected closing ```)");
  emit_plain(l, TK_ERROR);
}

// Whitespace and newlines ---------------------------------------------

static void lex_spaces(Lex *l) {
  while (curr(l) == ' ')
    advance(l);
  emit_plain(l, TK_SPACE);
}

// Handles \n, \r\n, and lone \r as one logical NEWLINE token.
static void lex_newline(Lex *l) {
  if (curr(l) == '\r') {
    advance(l);
    if (curr(l) == '\n')
      advance(l);
  } else {
    advance(l); // '\n'
  }
  emit_plain(l, TK_NEWLINE);
  record_line_start(l);
}

// =====================================================================
// Top-level dispatch — scans one token starting at l->pos, pushes to
// l->tokens. Pre: l->pos < l->source_len, l->tok_start == l->pos.
// =====================================================================

static void scan_one(Lex *l) {
  char c = curr(l);

  if (isdigit((unsigned char)c)) {
    lex_number(l);
    return;
  }
  if (is_id_start(c)) {
    lex_identifier_or_keyword(l);
    return;
  }

  switch (c) {
  case ' ':
    lex_spaces(l);
    return;
  case '\n':
  case '\r':
    lex_newline(l);
    return;

  case '\t':
    lex_error(l, "tab characters are not allowed; use spaces for indentation");
    advance(l);
    emit_plain(l, TK_ERROR);
    return;

  case '"':
    lex_string(l);
    return;
  case '\'':
    lex_byte_lit(l);
    return;
  case '`':
    if (peek_at(l, 1) == '`' && peek_at(l, 2) == '`') {
      lex_asm_lit(l);
      return;
    }
    lex_error(l,
              "single backtick is not a valid token; use ``` for inline asm");
    advance(l);
    emit_plain(l, TK_ERROR);
    return;

  // Single-char delimiters and sigils.
  case '(':
    advance(l);
    emit_plain(l, TK_LPAREN);
    return;
  case ')':
    advance(l);
    emit_plain(l, TK_RPAREN);
    return;
  case '[':
    advance(l);
    emit_plain(l, TK_LBRACKET);
    return;
  case ']':
    advance(l);
    emit_plain(l, TK_RBRACKET);
    return;
  case '{':
    advance(l);
    emit_plain(l, TK_LBRACE);
    return;
  case '}':
    advance(l);
    emit_plain(l, TK_RBRACE);
    return;
  case ';':
    advance(l);
    emit_plain(l, TK_SEMI);
    return;
  case ',':
    advance(l);
    emit_plain(l, TK_COMMA);
    return;
  case '@':
    advance(l);
    emit_plain(l, TK_AT);
    return;
  case '#':
    advance(l);
    emit_plain(l, TK_HASH);
    return;
  case '~':
    advance(l);
    emit_plain(l, TK_TILDE);
    return;
  case '?':
    advance(l);
    emit_plain(l, TK_QUESTION);
    return;

  case '^':
    advance(l);
    emit_plain(l, match(l, '=') ? TK_CARET_EQ : TK_CARET);
    return;

  case '+':
    advance(l);
    if (match(l, '='))
      emit_plain(l, TK_PLUS_EQ);
    else if (match(l, '+'))
      emit_plain(l, TK_PLUS_PLUS);
    else
      emit_plain(l, TK_PLUS);
    return;

  case '-':
    advance(l);
    if (match(l, '>'))
      emit_plain(l, TK_RARROW);
    else if (match(l, '='))
      emit_plain(l, TK_MINUS_EQ);
    else
      emit_plain(l, TK_MINUS);
    return;

  case '*':
    advance(l);
    if (match(l, '='))
      emit_plain(l, TK_STAR_EQ);
    else if (match(l, '*'))
      emit_plain(l, TK_STAR_STAR);
    else
      emit_plain(l, TK_STAR);
    return;

  case '%':
    advance(l);
    emit_plain(l, match(l, '=') ? TK_PERCENT_EQ : TK_PERCENT);
    return;

  case '&':
    advance(l);
    if (match(l, '&'))
      emit_plain(l, TK_AMP_AMP);
    else if (match(l, '='))
      emit_plain(l, TK_AMP_EQ);
    else
      emit_plain(l, TK_AMP);
    return;

  case '|':
    advance(l);
    if (match(l, '|'))
      emit_plain(l, TK_PIPE_PIPE);
    else if (match(l, '='))
      emit_plain(l, TK_PIPE_EQ);
    else
      emit_plain(l, TK_PIPE);
    return;

  case '!':
    advance(l);
    emit_plain(l, match(l, '=') ? TK_BANG_EQ : TK_BANG);
    return;

  case '=':
    advance(l);
    if (match(l, '='))
      emit_plain(l, TK_EQ_EQ);
    else if (match(l, '>'))
      emit_plain(l, TK_FATARROW);
    else
      emit_plain(l, TK_EQ);
    return;

  case '<':
    advance(l);
    if (match(l, '='))
      emit_plain(l, TK_LE);
    else if (match(l, '<'))
      emit_plain(l, TK_SHL);
    else if (match(l, '-'))
      emit_plain(l, TK_LARROW);
    else
      emit_plain(l, TK_LT);
    return;

  case '>':
    advance(l);
    if (match(l, '='))
      emit_plain(l, TK_GE);
    else if (match(l, '>'))
      emit_plain(l, TK_SHR);
    else
      emit_plain(l, TK_GT);
    return;

  case ':':
    advance(l);
    if (match(l, ':'))
      emit_plain(l, TK_COLON_COLON);
    else if (match(l, '='))
      emit_plain(l, TK_COLON_EQ);
    else
      emit_plain(l, TK_COLON);
    return;

  case '/':
    if (peek_at(l, 1) == '/') {
      lex_line_comment(l);
      return;
    }
    if (peek_at(l, 1) == '*') {
      lex_block_comment(l);
      return;
    }
    advance(l);
    emit_plain(l, match(l, '=') ? TK_SLASH_EQ : TK_SLASH);
    return;

  case '.':
    advance(l);
    if (!match(l, '.')) {
      emit_plain(l, TK_DOT);
      return;
    }
    if (match(l, '.'))
      emit_plain(l, TK_DOT_DOT_DOT);
    else
      emit_plain(l, TK_DOT_DOT);
    return;
  }

  // Unrecognized character — diagnose, advance one byte, emit Error.
  lex_error(l, "unexpected character");
  advance(l);
  emit_plain(l, TK_ERROR);
}

// =====================================================================
// Public entry
// =====================================================================

void lex(const char *source, uint32_t source_len, StringPool *pool,
         Vec *out_tokens, Vec *out_line_starts) {
  Lex l = {
      .source = source,
      .source_len = source_len,
      .pos = 0,
      .tok_start = 0,
      .pool = pool,
      .tokens = out_tokens,
      .line_starts = out_line_starts,
  };

  // Line 1 starts at byte 0.
  uint32_t zero = 0;
  vec_push(l.line_starts, &zero);

  // Skip a leading UTF-8 BOM if present. The BOM is invisible to the
  // user and shouldn't shift any byte offsets reported in diagnostics
  // for the first line.
  if (source_len >= 3 && (unsigned char)source[0] == 0xEF &&
      (unsigned char)source[1] == 0xBB && (unsigned char)source[2] == 0xBF) {
    l.pos = 3;
  }

  while (l.pos < l.source_len) {
    l.tok_start = l.pos;
    scan_one(&l);
  }

  // Final EOF marker at the end of source. byte_start == byte_end ==
  // source_len means "the position past the last byte" — useful for
  // diagnostics pointing at the implicit end-of-file.
  l.tok_start = l.pos;
  emit_plain(&l, TK_EOF);
}
