#include "./token.h"
#include <stddef.h> // For NULL

const char* token_kind_to_str(enum TokenKind kind) {
    switch (kind) {
        case Eof: return "Eof";
        case Error: return "Error";
        case Identifier: return "Identifier";
        case IntLit: return "IntLit";
        case FloatLit: return "FloatLit";
        case StringLit: return "StringLit";
        case ByteLit: return "ByteLit";
        case True: return "True";
        case False: return "False";
        case Void: return "Void";
        case Never: return "Never";
        case If: return "If";
        case Then: return "Then";
        case Else: return "Else";
        case With: return "With";
        case Return: return "Return";
        case For: return "For";
        case Break: return "Break";
        case Catch: return "Catch";
        case Try: return "Try";
        case Nil: return "Nil";
        case Or: return "Or";
        case Type: return "Type";
        case Data: return "Data";
        case Where: return "Where";
        case Extern: return "Extern";
        case Pvt: return "Pvt";
        case Effect: return "Effect";
        case Scoped: return "Scoped";
        case Named: return "Named";
        case In: return "In";
        case Handler: return "Handler";
        case Ctl: return "Ctl";
        case Final: return "Final";
        case Resume: return "Resume";
        case Override: return "Override";
        case Mask: return "Mask";
        case Forall: return "Forall";
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
        case Arrow: return "Arrow";
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
    }
    return NULL;
}

struct Span span_new(int start, int end, int line, int col) {
    struct Span span = {start, end, line, col};
    return span;
}
