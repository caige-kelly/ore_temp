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

    // Keywords - effects
    Effect,
    Scoped,
    Named,
    Handler,
    Handle,
    Resume,
    Override,
    Mask,
    Initally,
    Finally,
    In,

    // Keywords - types / polymorphism
    Forall,

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

    //Scope
    Dollar,

};


struct Span {
    int start;
    int end;
    int line;
    int column;
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
struct Span span_new(int start, int end, int line, int col);

// The TokenVec definitions and functions have been removed from here.
// We now use the generic Vec from common/vec.h


#endif // TOKEN_H
