#include "./lexer.h"
#include "../common/stringpool.h"
#include "./token.h"
#include <ctype.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

struct Lexer lexer_new(const char *source, int file_id) {
  struct Lexer l = {
      .source = source,
      .start = 0,
      .current = 0,
      .line = 1,
      .column = 1,
      .start_column = 1,
      .file_id = file_id,
      .at_line_start = true,
  };
  return l;
}

// Helper to advance the lexer by one character and update the column.
static void advance(struct Lexer *lexer) {
  if (lexer->source[lexer->current] == '\n') {
    lexer->line++;
    lexer->column = 1;
  } else {
    lexer->column++;
  }
  lexer->current++;
}

// Helper to create a token.
static struct Token make_token(struct Lexer *lexer, StringPool *pool,
                               enum TokenKind kind) {
  struct Span span = span_new(lexer->file_id, lexer->start, lexer->current,
                              lexer->line, lexer->start_column);

  size_t len = lexer->current - lexer->start;
  // For string literals, we don't want to include the quotes in the lexeme.
  if (kind == StringLit || kind == ByteLit) {
    struct Token t = {.kind = kind,
                      .string_id = pool_intern(
                          pool, &lexer->source[lexer->start + 1], len - 2),
                      .string_len = len - 2,
                      .span = span,
                      .origin = Source};
    return t;
  }

  if (kind == NewLine || kind == Eof) {
    struct Token t = {.kind = kind,
                      .string_id = pool_intern(pool, "", 0),
                      .string_len = 0,
                      .span = span,
                      .origin = Source};
    return t;
  }

  struct Token t = {.kind = kind,
                    .string_id =
                        pool_intern(pool, &lexer->source[lexer->start], len),
                    .string_len = len,
                    .span = span,
                    .origin = Source};
  return t;
}

// Helper to get the token kind for a given keyword string.
static enum TokenKind get_keyword_kind(const char *keyword) {
  if (strcmp(keyword, "if") == 0)
    return If;
  if (strcmp(keyword, "else") == 0)
    return Else;
  if (strcmp(keyword, "true") == 0)
    return True;
  if (strcmp(keyword, "false") == 0)
    return False;
  if (strcmp(keyword, "nil") == 0)
    return Nil;
  if (strcmp(keyword, "void") == 0)
    return Void;
  if (strcmp(keyword, "const") == 0)
    return Const;
  if (strcmp(keyword, "type") == 0)
    return Type;
  if (strcmp(keyword, "orelse") == 0)
    return OrElse;
  if (strcmp(keyword, "struct") == 0)
    return Struct;
  if (strcmp(keyword, "enum") == 0)
    return Enum;
  if (strcmp(keyword, "union") == 0)
    return Union;
  if (strcmp(keyword, "effect") == 0)
    return Effect;
  if (strcmp(keyword, "scoped") == 0)
    return Scoped;
  if (strcmp(keyword, "named") == 0)
    return Named;
  if (strcmp(keyword, "handler") == 0)
    return Handler;
  if (strcmp(keyword, "resume") == 0)
    return Resume;
  if (strcmp(keyword, "override") == 0)
    return Override;
  if (strcmp(keyword, "mask") == 0)
    return Mask;
  if (strcmp(keyword, "with") == 0)
    return With;
  if (strcmp(keyword, "comptime") == 0)
    return Comptime;
  if (strcmp(keyword, "pub") == 0)
    return Pub;
  if (strcmp(keyword, "noreturn") == 0)
    return NoReturn;
  if (strcmp(keyword, "switch") == 0)
    return Switch;
  if (strcmp(keyword, "continue") == 0)
    return Continue;
  if (strcmp(keyword, "handle") == 0)
    return Handle;
  if (strcmp(keyword, "finally") == 0)
    return Finally;
  if (strcmp(keyword, "initially") == 0)
    return Initially;
  if (strcmp(keyword, "anytype") == 0)
    return AnyType;
  if (strcmp(keyword, "elif") == 0)
    return Elif;
  if (strcmp(keyword, "in") == 0)
    return In;
  if (strcmp(keyword, "return") == 0)
    return Return;
  if (strcmp(keyword, "fn") == 0)
    return Fn;
  if (strcmp(keyword, "loop") == 0)
    return Loop;
  if (strcmp(keyword, "defer") == 0)
    return Defer;
  if (strcmp(keyword, "ctl") == 0)
    return Ctl;
  if (strcmp(keyword, "break") == 0)
    return Break;
  if (strcmp(keyword, "continue") == 0)
    return Continue;

  return Identifier;
}

// Reads a full identifier or keyword.
static struct Token identifier_or_keyword(struct Lexer *lexer,
                                          StringPool *pool) {
  while (isalnum(lexer->source[lexer->current]) ||
         lexer->source[lexer->current] == '_') {
    advance(lexer);
  }
  struct Token token = make_token(lexer, pool, Identifier);
  token.kind =
      get_keyword_kind(pool_get(pool, token.string_id, token.string_len));
  return token;
}

// Reads a number literal (integer or float).
// Reads a number literal (integer or float).
// Reads a number literal (integer or float).
static struct Token number(struct Lexer *lexer, StringPool *pool) {
  char c = lexer->source[lexer->current];
  char next = lexer->source[lexer->current + 1];

  // Alternate bases: 0x, 0X, 0b, 0B, 0o, 0O
  if (c == '0') {
    if (next == 'x' || next == 'X') {
      advance(lexer); // '0'
      advance(lexer); // 'x' or 'X'
      while (isxdigit(lexer->source[lexer->current]) ||
             lexer->source[lexer->current] == '_') {
        advance(lexer);
      }
      return make_token(lexer, pool, IntLit);
    }
    if (next == 'b' || next == 'B') {
      advance(lexer); // '0'
      advance(lexer); // 'b' or 'B'
      while (lexer->source[lexer->current] == '0' ||
             lexer->source[lexer->current] == '1' ||
             lexer->source[lexer->current] == '_') {
        advance(lexer);
      }
      return make_token(lexer, pool, IntLit);
    }
    if (next == 'o' || next == 'O') {
      advance(lexer); // '0'
      advance(lexer); // 'o' or 'O'
      while ((lexer->source[lexer->current] >= '0' &&
              lexer->source[lexer->current] <= '7') ||
             lexer->source[lexer->current] == '_') {
        advance(lexer);
      }
      return make_token(lexer, pool, IntLit);
    }
  }

  // Decimal integer (with underscores)
  while (isdigit(lexer->source[lexer->current]) ||
         lexer->source[lexer->current] == '_') {
    advance(lexer);
  }

  bool is_float = false;

  // Fractional part: .digits
  if (lexer->source[lexer->current] == '.' &&
      isdigit(lexer->source[lexer->current + 1])) {
    is_float = true;
    advance(lexer); // '.'
    while (isdigit(lexer->source[lexer->current]) ||
           lexer->source[lexer->current] == '_') {
      advance(lexer);
    }
  }

  // Exponent: e/E [+/-] digits
  if (lexer->source[lexer->current] == 'e' ||
      lexer->source[lexer->current] == 'E') {
    // Look ahead to make sure it's a valid exponent
    size_t lookahead = lexer->current + 1;
    if (lexer->source[lookahead] == '+' || lexer->source[lookahead] == '-') {
      lookahead++;
    }
    if (isdigit(lexer->source[lookahead])) {
      is_float = true;
      advance(lexer); // 'e' or 'E'
      if (lexer->source[lexer->current] == '+' ||
          lexer->source[lexer->current] == '-') {
        advance(lexer); // sign
      }
      while (isdigit(lexer->source[lexer->current]) ||
             lexer->source[lexer->current] == '_') {
        advance(lexer);
      }
    }
  }

  return make_token(lexer, pool, is_float ? FloatLit : IntLit);
}

// Helper to advance the lexer and return a token.
static struct Token advance_and_make_token(struct Lexer *lexer,
  StringPool *pool,
  enum TokenKind kind) {
  struct Token token = make_token(lexer, pool, kind);
  advance(lexer);
  return token;
}

static struct Token string(struct Lexer *lexer, StringPool *pool) {
  advance(lexer); // consume opening "
  while (lexer->source[lexer->current] != '"' &&
         lexer->source[lexer->current] != '\0' &&
         lexer->source[lexer->current] != '\n') {
    if (lexer->source[lexer->current] == '\\' &&
        lexer->source[lexer->current + 1] == '"') {
      advance(lexer);  // skip the escaped quote
    }
    advance(lexer);
  }
  if (lexer->source[lexer->current] != '"') {
    return make_token(lexer, pool, Error);  // unterminated (\0 or \n)
  }
  return advance_and_make_token(lexer, pool, StringLit);
}

static struct Token byte(struct Lexer *lexer, StringPool *pool) {
  advance(lexer); // consume opening '
  while (lexer->source[lexer->current] != '\'' &&
         lexer->source[lexer->current] != '\0' &&
         lexer->source[lexer->current] != '\n') {
    advance(lexer);
  }
  if (lexer->source[lexer->current] != '\'') {
    return make_token(lexer, pool, Error);
  }
  return advance_and_make_token(lexer, pool, ByteLit);
}

struct Token tokenizer(struct Lexer *lexer, StringPool *pool) {
  lexer->start = lexer->current;
  lexer->start_column = lexer->column;

  char c = lexer->source[lexer->current];

  if (c == '\0')
    return make_token(lexer, pool, Eof);

  if (isdigit(c))
    return number(lexer, pool);

  if (isalpha(c))
    return identifier_or_keyword(lexer, pool);

  switch (c) {
  case ' ':
    while (lexer->source[lexer->current] != '\0') {
      if (lexer->source[lexer->current + 1] == ' ') advance(lexer);
      else return advance_and_make_token(lexer, pool, Space);
    }
    return make_token(lexer, pool, Error);  // unterminated comment
  case '\t':
    printf("tabs are not allowed.");
    exit(1);
    break;
  case '"':
    return string(lexer, pool);
  case '\'':
    return byte(lexer, pool);
  case '\r':
    advance(lexer);
  case '\n':        
    lexer->at_line_start = true;
    return advance_and_make_token(lexer, pool, NewLine);
  case '(':
    return advance_and_make_token(lexer, pool, LParen);
  case ')':
    return advance_and_make_token(lexer, pool, RParen);
  case '[':
    return advance_and_make_token(lexer, pool, LBracket);
  case ']':
    return advance_and_make_token(lexer, pool, RBracket);
  case '{':
    return advance_and_make_token(lexer, pool, LBrace);
  case '}':
    return advance_and_make_token(lexer, pool, RBrace);
  case ';':
    return advance_and_make_token(lexer, pool, Semicolon);
  case ',':
    return advance_and_make_token(lexer, pool, Comma);
  case '@':
    return advance_and_make_token(lexer, pool, At);
  case '#':
    return advance_and_make_token(lexer, pool, Hash);
  case '~':
    return advance_and_make_token(lexer, pool, Tilde);
  case '+':
    if (lexer->source[lexer->current + 1] == '=') {
      advance(lexer);
      return advance_and_make_token(lexer, pool, PlusEqual);
    }
    if (lexer->source[lexer->current + 1] == '+') {
      advance(lexer);
      return advance_and_make_token(lexer, pool, PlusPlus);
    } else {
      return advance_and_make_token(lexer, pool, Plus);
    }
  case '-':
    if (lexer->source[lexer->current + 1] == '>') {
      advance(lexer);
      return advance_and_make_token(lexer, pool, RightArrow);
    } else if (lexer->source[lexer->current + 1] == '=') {
      advance(lexer);
      return advance_and_make_token(lexer, pool, MinusEqual);
    } else {
      return advance_and_make_token(lexer, pool, Minus);
    }
  case '*':
    if (lexer->source[lexer->current + 1] == '=') {
      advance(lexer);
      return advance_and_make_token(lexer, pool, StarEqual);
    }
    if (lexer->source[lexer->current + 1] == '*') {
      advance(lexer);
      return advance_and_make_token(lexer, pool, StarStar);
    }
    else {
      return advance_and_make_token(lexer, pool, Star);
    }
    case '/':
    if (lexer->source[lexer->current + 1] == '=') {
      advance(lexer);
      return advance_and_make_token(lexer, pool, ForwardSlashEqual);
    } else if (lexer->source[lexer->current + 1] == '*') {
      advance(lexer);  // consume '/'
      advance(lexer);  // consume '*'
      while (lexer->source[lexer->current] != '\0') {
        if (lexer->source[lexer->current] == '*' &&
            lexer->source[lexer->current + 1] == '/') {
          advance(lexer);  // consume '*'
          return advance_and_make_token(lexer, pool, Comment);  // consume '/' + emit
        }
        advance(lexer); // consume '/'
      }
      return make_token(lexer, pool, Error);  // unterminated comment
    } else {
      return advance_and_make_token(lexer, pool, ForwardSlash);
    }
  case '%':
    if (lexer->source[lexer->current + 1] == '=') {
      advance(lexer);
      return advance_and_make_token(lexer, pool, PercentEqual);
    } else {
      return advance_and_make_token(lexer, pool, Percent);
    }
  case '&':
    if (lexer->source[lexer->current + 1] == '&') {
      advance(lexer);
      return advance_and_make_token(lexer, pool, AmpersandAmpersand);
    } else {
      return advance_and_make_token(lexer, pool, Ampersand);
    }
  case '|':
    if (lexer->source[lexer->current + 1] == '|') {
      advance(lexer);
      return advance_and_make_token(lexer, pool, PipePipe);
    } else {
      return advance_and_make_token(lexer, pool, Pipe);
    }
  case '^':
    return advance_and_make_token(lexer, pool, Caret);
  case '=':
    if (lexer->source[lexer->current + 1] == '=') {
      advance(lexer);
      return advance_and_make_token(lexer, pool, EqualEqual);
    } else if (lexer->source[lexer->current + 1] == '>') {
      advance(lexer);
      return advance_and_make_token(lexer, pool, FatArrow);
    } else {
      return advance_and_make_token(lexer, pool, Equal);
    }
  case '!':
    if (lexer->source[lexer->current + 1] == '=') {
      advance(lexer);
      return advance_and_make_token(lexer, pool, BangEqual);
    } else {
      return advance_and_make_token(lexer, pool, Bang);
    }
  case '<':
    if (lexer->source[lexer->current + 1] == '=') {
      advance(lexer);
      return advance_and_make_token(lexer, pool, LessEqual);
    } else if (lexer->source[lexer->current + 1] == '<') {
      advance(lexer);
      return advance_and_make_token(lexer, pool, ShiftLeft);
    } else if (lexer->source[lexer->current + 1] == '-') {
      advance(lexer);
      return advance_and_make_token(lexer, pool, LeftArrow);
    } else {
      return advance_and_make_token(lexer, pool, Less);
    }
  case '>':
    if (lexer->source[lexer->current + 1] == '=') {
      advance(lexer);
      return advance_and_make_token(lexer, pool, GreaterEqual);
    } else if (lexer->source[lexer->current + 1] == '>') {
      advance(lexer);
      return advance_and_make_token(lexer, pool, ShiftRight);
    } else {
      return advance_and_make_token(lexer, pool, Greater);
    }
  case ':':
    if (lexer->source[lexer->current + 1] == ':') {
      advance(lexer);
      return advance_and_make_token(lexer, pool, ColonColon);
    } else if (lexer->source[lexer->current + 1] == '=') {
      advance(lexer);
      return advance_and_make_token(lexer, pool, ColonEqual);
    } else {
      return advance_and_make_token(lexer, pool, Colon);
    }
  case '.':
    if (lexer->source[lexer->current + 1] == '.') {
      advance(lexer);
      if (lexer->source[lexer->current + 1] == '.')
        return advance_and_make_token(lexer, pool, DotDotDot);
      return advance_and_make_token(lexer, pool, DotDot);
    } else {
      return advance_and_make_token(lexer, pool, Dot);
    }
  case '?':
    return advance_and_make_token(lexer, pool, Question);
  case '_':
    return advance_and_make_token(lexer, pool, Underscore);
  }

  advance(lexer);
  return make_token(lexer, pool, Error);
}
