#ifndef TOKEN_H
#define TOKEN_H

#include <stdbool.h>
#include <stddef.h>
#include "../common/vec.h" // <-- Include the generic vector

// All the type definitions that other files might need to know about.

enum TokenOrigin {
    Source,
    Layout,
};

enum TokenKind {
    // Special Tokens
    Eof,
    Error,

    // Literals
    Identifier,
    IntLit,
    FloatLit,
    StringLit,
    ByteLit,
    True,
    False,
    Void,        // Keyword alias for Unit

    // keyword - Bottom Type
    Never,

    // Keywords - reserved
    If,
    Then,
    Else,
    With,
    Return,
    For,
    Break,
    Catch,
    Try,
    Nil,
    Or,

    // Keywords - definitions
    Type,
    Data,
    Where,
    Extern,

    // Keyword - visibility
    Pvt,


    // Keywords - effects
    Effect,
    Scoped,
    Named,
    In,
    Handler,
    Ctl,
    Final,
    Resume,
    Override,
    Mask,

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

    // Operators - other
    Arrow,
    Colon,
    ColonColon,
    Dot,
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
    int start;
    int end;
    int line;
    int column;
};

struct Token {
    enum TokenKind kind;
    char* lexeme; // Owned by the token, must be freed.
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
