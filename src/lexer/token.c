#include "./token.h"

const char *token_kind_str(TokenKind kind) {
    switch (kind) {
    case TK_EOF:           return "eof";
    case TK_ERROR:         return "error";

    case TK_NEWLINE:       return "newline";
    case TK_SPACE:         return "space";
    case TK_COMMENT:       return "comment";

    case TK_IDENTIFIER:    return "identifier";
    case TK_INT_LIT:       return "int_lit";
    case TK_FLOAT_LIT:     return "float_lit";
    case TK_STRING_LIT:    return "string_lit";
    case TK_BYTE_LIT:      return "byte_lit";
    case TK_ASM_LIT:       return "asm_lit";

    case TK_TRUE:          return "true";
    case TK_FALSE:         return "false";
    case TK_NIL:           return "nil";
    case TK_VOID:          return "void";

    case TK_FN:            return "fn";
    case TK_FN_TYPE:       return "Fn";
    case TK_TYPE:          return "type";
    case TK_CONST:         return "const";
    case TK_STRUCT:        return "struct";
    case TK_ENUM:          return "enum";
    case TK_UNION:         return "union";
    case TK_EFFECT:        return "effect";
    case TK_HANDLER:       return "handler";
    case TK_PUB:           return "pub";
    case TK_COMPTIME:      return "comptime";
    case TK_ANYTYPE:       return "anytype";
    case TK_NORETURN:      return "noreturn";

    case TK_IF:            return "if";
    case TK_ELIF:          return "elif";
    case TK_ELSE:          return "else";
    case TK_LOOP:          return "loop";
    case TK_SWITCH:        return "switch";
    case TK_BREAK:         return "break";
    case TK_CONTINUE:      return "continue";
    case TK_RETURN:        return "return";
    case TK_DEFER:         return "defer";
    case TK_ORELSE:        return "orelse";

    case TK_HANDLE:        return "handle";
    case TK_MASK:          return "mask";
    case TK_WITH:          return "with";

    case TK_AMP_AMP:       return "&&";
    case TK_PIPE_PIPE:     return "||";
    case TK_BANG:          return "!";

    case TK_PLUS:          return "+";
    case TK_MINUS:         return "-";
    case TK_STAR:          return "*";
    case TK_STAR_STAR:     return "**";
    case TK_SLASH:         return "/";
    case TK_PERCENT:       return "%";

    case TK_PIPE:          return "|";
    case TK_AMP:           return "&";
    case TK_CARET:         return "^";
    case TK_SHL:           return "<<";
    case TK_SHR:           return ">>";

    case TK_EQ_EQ:         return "==";
    case TK_BANG_EQ:       return "!=";
    case TK_LT:            return "<";
    case TK_LE:            return "<=";
    case TK_GT:            return ">";
    case TK_GE:            return ">=";

    case TK_EQ:            return "=";
    case TK_PLUS_EQ:       return "+=";
    case TK_MINUS_EQ:      return "-=";
    case TK_STAR_EQ:       return "*=";
    case TK_SLASH_EQ:      return "/=";
    case TK_PERCENT_EQ:    return "%=";
    case TK_PIPE_EQ:       return "|=";
    case TK_AMP_EQ:        return "&=";
    case TK_CARET_EQ:      return "^=";
    case TK_COLON_EQ:      return ":=";
    case TK_PLUS_PLUS:     return "++";

    case TK_RARROW:        return "->";
    case TK_LARROW:        return "<-";
    case TK_FATARROW:      return "=>";
    case TK_COLON:         return ":";
    case TK_COLON_COLON:   return "::";
    case TK_DOT:           return ".";
    case TK_DOT_DOT:       return "..";
    case TK_DOT_DOT_DOT:   return "...";
    case TK_QUESTION:      return "?";
    case TK_UNDERSCORE:    return "_";

    case TK_LPAREN:        return "(";
    case TK_RPAREN:        return ")";
    case TK_LBRACKET:      return "[";
    case TK_RBRACKET:      return "]";
    case TK_LBRACE:        return "{";
    case TK_RBRACE:        return "}";
    case TK_SEMI:          return ";";
    case TK_COMMA:         return ",";
    case TK_AT:            return "@";
    case TK_HASH:          return "#";
    case TK_TILDE:         return "~";

    case TK_COUNT:         break;  // sentinel; not a real kind
    }
    return "?";
}
