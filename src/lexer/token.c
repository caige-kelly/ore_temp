#include "./token.h"

const char *token_kind_str(SyntaxKind kind) {
  switch (kind) {
  case SK_EOF:
    return "eof";
  case SK_LEX_ERROR:
    return "error";

  case SK_NEWLINE:
    return "newline";
  case SK_WHITESPACE:
    return "space";
  case SK_COMMENT:
    return "comment";

  case SK_IDENT:
    return "identifier";
  case SK_INT_LIT:
    return "int_lit";
  case SK_FLOAT_LIT:
    return "float_lit";
  case SK_STRING_LIT:
    return "string_lit";
  case SK_BYTE_LIT:
    return "byte_lit";
  case SK_ASM_LIT:
    return "asm_lit";

  case SK_TRUE_KW:
    return "true";
  case SK_FALSE_KW:
    return "false";
  case SK_NIL_KW:
    return "nil";

  case SK_FN_KW:
    return "fn";
  case SK_FN_TYPE_KW:
    return "Fn";
  case SK_CONST_KW:
    return "const";
  case SK_STRUCT_KW:
    return "struct";
  case SK_ENUM_KW:
    return "enum";
  case SK_UNION_KW:
    return "union";
  case SK_EFFECT_KW:
    return "effect";
  case SK_HANDLER_KW:
    return "handler";
  case SK_COMPTIME_KW:
    return "comptime";

  case SK_IF_KW:
    return "if";
  case SK_ELIF_KW:
    return "elif";
  case SK_ELSE_KW:
    return "else";
  case SK_LOOP_KW:
    return "loop";
  case SK_SWITCH_KW:
    return "switch";
  case SK_BREAK_KW:
    return "break";
  case SK_CONTINUE_KW:
    return "continue";
  case SK_RETURN_KW:
    return "return";
  case SK_DEFER_KW:
    return "defer";
  case SK_ORELSE_KW:
    return "orelse";

  case SK_HANDLE_KW:
    return "handle";
  case SK_MASK_KW:
    return "mask";
  case SK_WITH_KW:
    return "with";

  case SK_AMP_AMP:
    return "&&";
  case SK_PIPE_PIPE:
    return "||";
  case SK_BANG:
    return "!";

  case SK_PLUS:
    return "+";
  case SK_MINUS:
    return "-";
  case SK_STAR:
    return "*";
  case SK_STAR_STAR:
    return "**";
  case SK_SLASH:
    return "/";
  case SK_PERCENT:
    return "%";

  case SK_PIPE:
    return "|";
  case SK_AMP:
    return "&";
  case SK_CARET:
    return "^";
  case SK_SHL:
    return "<<";
  case SK_SHR:
    return ">>";

  case SK_EQ_EQ:
    return "==";
  case SK_BANG_EQ:
    return "!=";
  case SK_LT:
    return "<";
  case SK_LE:
    return "<=";
  case SK_GT:
    return ">";
  case SK_GE:
    return ">=";

  case SK_EQ:
    return "=";
  case SK_PLUS_EQ:
    return "+=";
  case SK_MINUS_EQ:
    return "-=";
  case SK_STAR_EQ:
    return "*=";
  case SK_SLASH_EQ:
    return "/=";
  case SK_PERCENT_EQ:
    return "%=";
  case SK_PIPE_EQ:
    return "|=";
  case SK_AMP_EQ:
    return "&=";
  case SK_TILDE_EQ:
    return "~=";
  case SK_COLON_EQ:
    return ":=";
  case SK_PLUS_PLUS:
    return "++";

  case SK_RARROW:
    return "->";
  case SK_LARROW:
    return "<-";
  case SK_FATARROW:
    return "=>";
  case SK_COLON:
    return ":";
  case SK_COLON_COLON:
    return "::";
  case SK_DOT:
    return ".";
  case SK_DOT_DOT:
    return "..";
  case SK_DOT_DOT_DOT:
    return "...";
  case SK_QUESTION:
    return "?";
  case SK_UNDERSCORE:
    return "_";

  case SK_LPAREN:
    return "(";
  case SK_RPAREN:
    return ")";
  case SK_LBRACKET:
    return "[";
  case SK_RBRACKET:
    return "]";
  case SK_LBRACE:
    return "{";
  case SK_RBRACE:
    return "}";
  case SK_SEMI:
    return ";";
  case SK_COMMA:
    return ",";
  case SK_AT:
    return "@";
  case SK_HASH:
    return "#";
  case SK_TILDE:
    return "~";

  case SK_LAST_TOKEN_KIND:
    break; // sentinel; not a real kind
  }
  return "?";
}
