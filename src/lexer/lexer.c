#include "./lexer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../db/storage/stringpool.h"
#include "../db/storage/vec.h"
#include "./token.h"

// =====================================================================
// Internal lex state == the public streaming cursor. All sub-lexers
// take `Lex *`; `Lex` is just the in-translation-unit name for
// `LexCursor` (declared in lexer.h). scan_one() fills `l->pending`
// (exactly one token per call) instead of pushing to a Vec; lex() and
// lex_next() read it from there.
// =====================================================================

typedef LexCursor Lex;

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

// Sentinel-scan read of the current byte, WITHOUT the per-byte
// pos<source_len bounds branch that curr()/peek_at() carry.
//
// Safe because dup_source_text mallocs len+1 and writes copy[len]='\0'
// — exactly one byte of slack, always a NUL. The predicate-driven
// inner scan loops that use this (decimal/hex/binary/octal digits,
// identifier continuation) all have predicates that REJECT '\0', so:
//   * the loop halts at the sentinel byte exactly where curr()/
//     advance() would have halted at EOF — byte-identical behavior;
//   * it never reads past source[source_len] (pos only advances while
//     the predicate held, i.e. only while pos<source_len), so the
//     single slack byte is sufficient;
//   * `l->pos++` replaces advance() inside these loops: advance() only
//     ran when the predicate held ⟹ pos<source_len ⟹ advance() was
//     exactly pos++ there, so this is equivalent, not a behavior change.
// NOT for lookahead (peek_at(l,1)) or terminator-seeking loops
// (string/comment/asm) — those keep the clamped accessors.
static inline char scur(const Lex *l) { return l->source[l->pos]; }

static inline bool is_id_start(char c) {
  unsigned char u = (unsigned char)c;
  return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u == '_';
}

// Direct ASCII range checks. Replaces libc isdigit/isxdigit, which on
// Darwin route through locale-aware __maskrune (a table indirection /
// call) — pure overhead here since Ore source is ASCII and these sit on
// the hot path (is_digit is scan_one's first test, every token). The
// classification is locale-independent by language definition, so this
// is byte-identical, not a behavior change.
static inline bool is_digit(char c) {
  unsigned char u = (unsigned char)c;
  return u >= '0' && u <= '9';
}

static inline bool is_hex_digit(char c) {
  unsigned char u = (unsigned char)c;
  return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'f') ||
         (u >= 'A' && u <= 'F');
}

static inline bool is_id_cont(char c) { return is_id_start(c) || is_digit(c); }

// =====================================================================
// Token emission
// =====================================================================

static void emit(Lex *l, TokenKind kind, StrId sid) {
  l->pending = (Token){
      .kind = kind,
      ._pad0 = 0,
      .string_id = sid,
      .start = l->tok_start,
      .byte_end = l->pos,
  };
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
  *(uint32_t *)vec_push_slot(l->line_starts) = l->pos;
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
// Dispatched by length, then a single memcmp against the same-length
// keyword(s). `default` instantly rejects identifiers of length <2 or
// >8 (no keyword there). This replaced a linear scan over all ~28
// entries (a `len ==` test + memcmp per entry) run on every
// identifier; behavior is identical (this switch IS the keyword set —
// reserved keywords only; contextual keywords val/final/raw/ctl/
// override/named/in/scoped/linear are intentionally absent and lex as
// TK_IDENTIFIER for the parser to disambiguate).
//
// MK(literal, KIND): confirm the lexeme against one same-length
// keyword. `len` already equals the case label, so memcmp of `len`
// bytes against the equal-length literal is an exact full compare.
#define MK(lit, KIND)                                                          \
  if (memcmp(s, (lit), len) == 0)                                              \
  return KIND

static TokenKind keyword_kind(const char *s, uint32_t len) {
  switch (len) {
  case 2:
    switch (s[0]) {
    case 'i':
      MK("if", TK_IF);
      break;
    case 'f':
      MK("fn", TK_FN);
      break;
    case 'F':
      MK("Fn", TK_FN_TYPE);
      break;
    }
    break;
  case 3:
    MK("nil", TK_NIL);
    break;
  case 4:
    switch (s[0]) {
    case 'e':
      MK("elif", TK_ELIF);
      MK("else", TK_ELSE);
      MK("enum", TK_ENUM);
      break;
    case 't':
      MK("true", TK_TRUE);
      break;
    case 'l':
      MK("loop", TK_LOOP);
      break;
    case 'w':
      MK("with", TK_WITH);
      break;
    case 'm':
      MK("mask", TK_MASK);
      break;
    }
    break;
  case 5:
    switch (s[0]) {
    case 'f':
      MK("false", TK_FALSE);
      break;
    case 'c':
      MK("const", TK_CONST);
      break;
    case 'b':
      MK("break", TK_BREAK);
      break;
    case 'd':
      MK("defer", TK_DEFER);
      break;
    case 'u':
      MK("union", TK_UNION);
      break;
    }
    break;
  case 6:
    switch (s[0]) {
    case 's':
      MK("struct", TK_STRUCT);
      MK("switch", TK_SWITCH);
      break;
    case 'e':
      MK("effect", TK_EFFECT);
      break;
    case 'r':
      MK("return", TK_RETURN);
      break;
    case 'o':
      MK("orelse", TK_ORELSE);
      break;
    case 'h':
      MK("handle", TK_HANDLE);
      break;
    }
    break;
  case 7:
    switch (s[0]) {
    case 'h':
      MK("handler", TK_HANDLER);
      break;
    }
    break;
  case 8:
    switch (s[0]) {
    case 'c':
      MK("comptime", TK_COMPTIME);
      MK("continue", TK_CONTINUE);
      break;
    }
    break;
  }
  return TK_IDENTIFIER;
}

#undef MK

// =====================================================================
// Sub-lexers
// =====================================================================

static void lex_identifier_or_keyword(Lex *l) {
  // Kebab identifiers (Koka @idchar rule): a `-` continues the
  // identifier iff the char right after it is an id-char. So
  // `foo-bar` is ONE identifier; `a - b`, `x-=y`, and a trailing
  // `foo-` all stop before the `-` (it then lexes as the minus
  // operator — that path is untouched). Accepted cost: `x-1` is the
  // identifier `x-1`; binary minus must be spaced (`a - b`).
  // `_` is already an id-char; bare `_` stays the wildcard below.
  // (An 8-byte SWAR chunk fast-path was tried here and reverted: real
  // identifiers are mostly <8 bytes so the chunk rarely fires, leaving
  // its bounds-check + 8-way is_id_cont test as pure overhead in front
  // of a scalar loop that already wins — measured ~3% regression on
  // dense/plain, a wash on id-heavy input. ws-SWAR keeps paying because
  // whitespace runs are genuinely long; identifier runs are not.)
  while (is_id_cont(scur(l)) ||
         (scur(l) == '-' && is_id_cont(peek_at(l, 1))))
    l->pos++;
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
  while (is_digit(scur(l)) || scur(l) == '_')
    l->pos++;
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
      while (is_hex_digit(scur(l)) || scur(l) == '_')
        l->pos++;
    } else if (c1 == 'b' || c1 == 'B') {
      while (scur(l) == '0' || scur(l) == '1' || scur(l) == '_')
        l->pos++;
    } else {
      while ((scur(l) >= '0' && scur(l) <= '7') || scur(l) == '_')
        l->pos++;
    }
    emit_interned(l, TK_INT_LIT);
    return;
  }

  scan_decimal_digits(l);

  bool is_float = false;

  // Fractional part: only if the dot is followed by a digit. So
  // `1..2` (range) and `1.foo` (postfix) still parse correctly.
  if (curr(l) == '.' && is_digit(peek_at(l, 1))) {
    is_float = true;
    advance(l);
    scan_decimal_digits(l);
  }

  // Exponent: e/E [+/-] digits — at least one digit required.
  if (curr(l) == 'e' || curr(l) == 'E') {
    uint32_t look = 1;
    if (peek_at(l, look) == '+' || peek_at(l, look) == '-')
      look++;
    if (is_digit(peek_at(l, look))) {
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
          if (!is_hex_digit(curr(l))) {
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
  // SWAR: skip full 8-byte runs of ' ' at once. Byte-identical to the
  // scalar loop — it only advances over chunks that are ENTIRELY 0x20
  // (single-value equality, endianness-irrelevant); the boundary chunk
  // and the <8 tail fall to the scalar loop, which stops at the exact
  // same first non-space. Strictly in-bounds: an 8-byte load only when
  // pos+8 <= source_len, so it never relies on the 1-byte NUL slack
  // (source contract guarantees only source[source_len]=='\0').
  const char *src = l->source;
  uint32_t n = l->source_len;
  while ((uint64_t)l->pos + 8u <= (uint64_t)n) {
    uint64_t w;
    memcpy(&w, src + l->pos, 8);
    if (w != 0x2020202020202020ULL)
      break;
    l->pos += 8;
  }
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
// Top-level dispatch — scans one token starting at l->pos and stores
// it in l->pending. Emits EXACTLY ONE token per call (every path ends
// in a single emit*()); the streaming cursor relies on this 1:1.
// Pre: l->pos < l->source_len, l->tok_start == l->pos.
// =====================================================================

static void scan_one(Lex *l) {
  char c = curr(l);

  if (is_digit(c)) {
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

void lex_begin(LexCursor *c, const char *source, uint32_t source_len,
               StringPool *pool, Vec *out_line_starts) {
  *c = (LexCursor){
      .source = source,
      .source_len = source_len,
      .pos = 0,
      .tok_start = 0,
      .pool = pool,
      .line_starts = out_line_starts,
      .pending = (Token){0},
      .eof_emitted = false,
  };

  // Skip a leading UTF-8 BOM if present. The BOM is invisible to the
  // user; the editor reports the first real character at column 0, so
  // line 1's "start" for column derivation must be the byte AFTER the
  // BOM, not byte 0. Order matters here: record line 1 only after
  // adjusting for the BOM.
  if (source_len >= 3 && (unsigned char)source[0] == 0xEF &&
      (unsigned char)source[1] == 0xBB && (unsigned char)source[2] == 0xBF) {
    c->pos = 3;
  }
  vec_push(c->line_starts, &c->pos);
}

Token lex_next(LexCursor *c) {
  // Idempotent at end of stream: once EOF is produced, every further
  // call returns that same EOF token.
  if (c->eof_emitted)
    return c->pending;

  if (c->pos >= c->source_len) {
    // Final EOF marker. byte_start == byte_end == source_len means
    // "the position past the last byte" — useful for diagnostics
    // pointing at the implicit end-of-file.
    c->tok_start = c->pos;
    emit_plain(c, TK_EOF);
    c->eof_emitted = true;
    return c->pending;
  }

  c->tok_start = c->pos;
  scan_one(c); // fills exactly one token into c->pending
  return c->pending;
}

// Batch wrapper over the streaming cursor. Output is byte-identical to
// pulling lex_next() to EOF: same token sequence, same line_starts.
void lex(const char *source, uint32_t source_len, StringPool *pool,
         Vec *out_tokens, Vec *out_line_starts) {
  LexCursor c;
  lex_begin(&c, source, source_len, pool, out_line_starts);
  for (;;) {
    Token t = lex_next(&c);
    vec_push(out_tokens, &t);
    if (t.kind == TK_EOF)
      break;
  }
}
