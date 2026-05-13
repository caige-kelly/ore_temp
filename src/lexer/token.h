#ifndef TOKEN_H
#define TOKEN_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "../db/storage/stringpool.h"

// All the type definitions that other files might need to know about.

enum TokenOrigin {
    Source,
    Layout,
};

enum TokenKind {
    // Special Tokens
    Eof,
    Error,
    NewLine,
    Space,
    Comment,

    // Literals
    Identifier,
    IntLit,
    FloatLit,
    StringLit,
    ByteLit,
    AsmLit,
    True,
    False,
    Void,
    Return,
    Fn,
    FnType,
    Ctl,
    Defer,
    NoReturn,
    Switch,

    // Keywords - loop
    Break,
    Continue,
    Loop,

    // Keywords - reserved
    If,
    Elif,
    Else,
    With,
    Nil,
    OrElse,
    Const,

    // Keywords - definitions
    AnyType,
    Comptime,
    Type,
    Struct,
    Enum,
    Union,
    Pub,

    // Keywords - effects
    Val,
    Final,
    Raw,
    Effect,
    Scoped,
    Linear,
    Named,
    Handler,
    Handle,
    Override,
    Mask,
    In,

    // Operators - logical
    AmpersandAmpersand,
    PipePipe,
    Bang,

    // Operators - arithmetic
    Plus,
    Minus,
    Star,
    StarStar,
    ForwardSlash,
    Percent,

    // Operators - bitwise
    Pipe,
    Ampersand,
    Caret,
    ShiftLeft,
    ShiftRight,

    // Operators - relational
    EqualEqual,
    BangEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,

    // Operators - assignment
    Equal,
    PlusEqual,
    MinusEqual,
    StarEqual,
    ForwardSlashEqual,
    PercentEqual,
    PipeEqual,
    AmpersandEqual,
    CaretEqual,
    ColonEqual,
    PlusPlus,

    // Operators - other
    RightArrow,
    LeftArrow,
    FatArrow,
    Colon,
    ColonColon,
    Dot,
    DotDot,
    DotDotDot,
    Question,
    Underscore,

    // Delimiters
    LParen,
    RParen,
    LBracket,
    RBracket,
    LBrace,
    RBrace,
    Semicolon,
    Comma,
    At,
    Hash,
    Tilde,
};


struct Span {
    int file_id;
    int start;
    int end;
    int line;
    int line_end;
    int column;
    int column_end;
};

struct Token {
    uint8_t kind;      // TokenKind (enum)
    StrId string_id;   // Foreign Key to StringPool
    uint32_t len;      // Use uint32_t to keep Token small/packed
    Span span;         // Range in the source file
};

// A function to get a string representation of a token kind (for debugging).
const char* token_kind_to_str(enum TokenKind kind);

// A function to create a new span.
struct Span span_new(int file_id, int start, int end, int col_start, int col_end, int line, int line_end);

// The TokenVec definitions and functions have been removed from here.
// We now use the generic Vec from common/vec.h


#endif // TOKEN_H
