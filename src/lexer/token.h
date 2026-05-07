#ifndef TOKEN_H
#define TOKEN_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

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
    Abstract,

    // Keywords - effects
    Val,
    Final,
    Effect,
    Scoped,
    Linear,
    Named,
    Handler,
    Handle,
    Resume,
    Override,
    Mask,
    Initially,
    Finally,
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
    enum TokenKind kind;
    uint32_t string_id;
    size_t string_len;
    struct Span span;
    enum TokenOrigin origin;
};

// A function to get a string representation of a token kind (for debugging).
const char* token_kind_to_str(enum TokenKind kind);

// A function to create a new span.
struct Span span_new(int file_id, int start, int end, int col_start, int col_end, int line, int line_end);

// The TokenVec definitions and functions have been removed from here.
// We now use the generic Vec from common/vec.h


#endif // TOKEN_H
