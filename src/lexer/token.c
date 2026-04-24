#include "./token.h"
#include <stddef.h> // For NULL

const char* token_kind_to_str(enum TokenKind kind) {
    switch (kind) {
        // special
        case Eof: return "Eof";
        case Error: return "Error";

        // Literals
        case Identifier: return "Identifier";
        case IntLit: return "IntLit";
        case FloatLit: return "FloatLit";
        case StringLit: return "StringLit";
        case ByteLit: return "ByteLit";

        // bool
        case True: return "True";
        case False: return "False";

        // special return types
        case Void: return "Void";
        case NoReturn: return "NoReturn";

        // control flow
        case If: return "If";
        case Elif: return "ElIf";
        case Then: return "Then";
        case Else: return "Else";
        case Switch: return "Switch";
        case In: return "In";

        // loops
        case For: return "For";
        case Where: return "Where";
        case Break: return "Break";
        case Continue: return "Continue";
        case While: return "While";

        // Optional
        case Nil: return "Nil";
        case OrElse: return "OrElse";

        // Comptime
        case Type: return "Type";
        case Comptime: return "Comptime";
        case AnyType: return "AnyType";

        // Constructs
        case Struct: return "Struct";
        case Enum: return "Enum";
        case Union: return "Union";

        //Effects
        case With: return "With";
        case Effect: return "Effect";
        case Scoped: return "Scoped";
        case Named: return "Named";
        case Handler: return "Handler";
        case Handle: return "handler";
        case Resume: return "Resume";
        case Override: return "Override";
        case Mask: return "Mask";
        case Forall: return "Forall";
        case Finally: return "Finally";
        case Initally: return "Initally";

        // Sigils
        case AmpersandAmpersand: return "AmpersandAmpersand";
        case PipePipe: return "PipePipe";
        case Bang: return "Bang";
        case Plus: return "Plus";
        case Minus: return "Minus";
        case Star: return "Star";
        case StarStar: return "StarStar";
        case ForwardSlash: return "ForwardSlash";
        case Percent: return "Percent";
        case Pipe: return "Pipe";
        case Ampersand: return "Ampersand";
        case Caret: return "Caret";
        case EqualEqual: return "EqualEqual";
        case BangEqual: return "BangEqual";
        case Less: return "Less";
        case LessEqual: return "LessEqual";
        case Greater: return "Greater";
        case GreaterEqual: return "GreaterEqual";
        case Equal: return "Equal";
        case PlusEqual: return "PlusEqual";
        case MinusEqual: return "MinusEqual";
        case StarEqual: return "StarEqual";
        case ForwardSlashEqual: return "ForwardSlashEqual";
        case PercentEqual: return "PercentEqual";
        case PipeEqual: return "PipeEqual";
        case AmpersandEqual: return "AmpersandEqual";
        case CaretEqual: return "CaretEqual";
        case Colon: return "Colon";
        case ColonColon: return "ColonColon";
        case LParen: return "LParen";
        case RParen: return "RParen";
        case LBracket: return "LBracket";
        case RBracket: return "RBracket";
        case LBrace: return "LBrace";
        case RBrace: return "RBrace";
        case Semicolon: return "Semicolon";
        case Comma: return "Comma";
        case At: return "At";
        case Hash: return "Hash";
        case Tilde: return "Tilde";
        case Dot: return "Dot";
        case Question: return "Question";
        case Underscore: return "Underscore";
        case ShiftRight: return "ShiftRight";
        case ShiftLeft: return "ShiftLeft";
        case RightArrow: return "RightArrow";
        case FatArrow: return "FatArrow";
        case LeftArrow: return "LeftArrow";
        case DotDot: return "DotDot";
        case ColonEqual: return "ColonEqual";
        case Const: return "Const";
        case NewLine: return "NewLine";
        case PlusPlus: return "PlusPlus";
        case Dollar: return "Dollar";
    }
    return NULL;
}

struct Span span_new(int start, int end, int line, int col) {
    struct Span span = {start, end, line, col};
    return span;
}
