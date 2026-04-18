#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "ast.h"
#include "../lexer/token.h"
#include "common/arena.h"
#include "common/vec.h"
#include "./parser.h"

// -- Pratt section --

enum Precedence {
    PREC_NONE = 0,
    PREC_ASSIGN,
    PREC_OR,
    PREC_AND,
    PREC_EQUALITY,
    PREC_COMPARISON,
    PREC_RANGE,
    PREC_BITWISE,
    PREC_SHIFT,
    PREC_TERM,
    PREC_FACTOR,
    PREC_POWER,
    PREC_UNARY,
    PREC_POSTFIX,
};

static enum Precedence get_precedence(enum TokenKind kind) {
    switch(kind) {
        case LeftArrow: return PREC_ASSIGN;
        case PipePipe: return PREC_OR;
        case AmpersandAmpersand: return PREC_AND;
        case EqualEqual: return PREC_EQUALITY;
        case Less: case Greater: case LessEqual: case GreaterEqual: return PREC_COMPARISON;
        case DotDot: return PREC_RANGE;
        case Pipe: case Ampersand: case Caret: return PREC_BITWISE;
        case Plus: case Minus: return PREC_TERM;
        case Star: case ForwardSlash: case Percent: return PREC_FACTOR;
        case StarStar: return PREC_POWER;
        default: return PREC_NONE;
    }
}


// -- Init --

struct Parser parser_new(Vec* tokens, StringPool* pool) {
    Arena* a = malloc(sizeof(Arena));
    arena_init(a, tokens->count * sizeof(struct Expr) * 2);
    struct Parser p = {
        .tokens = tokens,
        .current = 0,
        .pool = pool,
        .arena = a,
    };

    return p;
}

// -- Helpers --

static struct Token* peek(struct Parser* p) {
    return (struct Token*)vec_get(p->tokens, p->current);
}

static struct Token* advance(struct Parser* p) {
    struct Token* t = peek(p);
    p->current++;
    return t;
}

static bool check(struct Parser* p, enum TokenKind kind) {
    struct Token* t = peek(p);
    return t && t-> kind == kind;
}

static bool match(struct Parser* p, enum TokenKind kind) {
    if (check(p, kind)) {
        advance(p);
        return true;
    }
    return false;
}

static struct Token* expect(struct Parser* p, enum TokenKind kind) {
    if (check(p, kind)) {
        return advance(p);
    }

    struct Token* t = peek(p);
    fprintf(stderr, "error: expected %s, got %s (line %d col %d)\n", 
        token_kind_to_str(kind),
        token_kind_to_str(t->kind),
        t->span.line, t->span.column);

    return NULL;
}

static struct Expr* alloc_expr(struct Parser* p, enum ExprKind kind, struct Span span) {
    struct Expr* e = arena_alloc(p->arena, sizeof(struct Expr));
    e->kind = kind;
    e->span = span;
    return e;
}

// -- Parse Functions --

static struct Expr* parse_expr_prec(struct Parser* p, enum Precedence min_prec);

static struct Expr* parse_primary(struct Parser* p) {
    struct Token* t = peek(p);

    switch (t->kind) {
        // Literals
        case IntLit: {
            advance(p);
            struct Expr* e = alloc_expr(p, expr_Lit, t->span);
            e->lit.kind = lit_Int;
            e->lit.string_id = t->string_id;
            return e;
        }
        case FloatLit: {
            advance(p);
            struct Expr* e = alloc_expr(p, expr_Lit, t->span);
            e->lit.kind = lit_Float;
            e->lit.string_id = t->string_id;
            return e;
        }
        case StringLit: {
            advance(p);
            struct Expr* e = alloc_expr(p, expr_Lit, t->span);
            e->lit.kind = lit_String;
            e->lit.string_id = t-> string_id;
            return e;
        }
        case ByteLit: {
            advance(p);
            struct Expr* e = alloc_expr(p, expr_Lit, t->span);
            e->lit.kind = lit_Byte;
            e->lit.string_id = t->string_id;
            return e;
        }
        case True: {
            advance(p);
            struct Expr* e = alloc_expr(p, expr_Lit, t->span);
            e->lit.kind = lit_True;
            e->lit.string_id = t->string_id;
            return e;
        }
        case False: {
            advance(p);
            struct Expr* e = alloc_expr(p, expr_Lit, t->span);
            e->lit.kind = lit_False;
            e->lit.string_id = t->string_id;
            return e;
        }
        case Nil: {
            advance(p);
            struct Expr* e = alloc_expr(p, expr_Ident, t->span);
            e->ident.string_id = t->string_id;
            e->ident.span = t->span;
            return e;
        }
        case Identifier: {
            advance(p);

            // x :: expr (const bind)
            if (check(p, ColonColon)) {
                advance(p);
                struct Expr* value = parse_expr_prec(p, PREC_NONE);
                struct Expr* e = alloc_expr(p, expr_Bind, t->span);
                e->bind.kind = bind_Const;
                e->bind.name = (struct Identifier){ .string_id=t->string_id, .span=t->span };
                e->bind.type_ann = NULL;
                e->bind.value = value;
                return e;
            }

            // x := expr (var bind)
            if (check(p, ColonEqual)) {
                advance(p);
                struct Expr* value = parse_expr_prec(p, PREC_NONE);
                struct Expr* e = alloc_expr(p, expr_Bind, t->span);
                e->bind.kind = bind_Const;
                e->bind.name = (struct Identifier){ .string_id=t->string_id, .span=t->span };
                e->bind.type_ann = NULL;
                e->bind.value = value;
                return e;
            }

            // x : T = expr (typed var) or x : T : expr (typed const)
            if (check(p, Colon)) {
                advance(p);
                struct Expr* type = parse_expr_prec(p, PREC_NONE);

                enum BindKind kind;
                if (match(p, Equal)) {
                    kind = bind_Var;
                } else if (match(p, Colon)) {
                    kind = bind_Const;
                } else {
                    fprintf(stderr, "error: expected '=' or ':' after type annotation\n");
                    return NULL;
                }

                struct Expr* value = parse_expr_prec(p, PREC_NONE);
                struct Expr* e = alloc_expr(p, expr_Bind, t->span);
                e->bind.kind = kind;
                e->bind.name = (struct Identifier){ .string_id=t->string_id, .span=t->span };
                e->bind.type_ann = type;
                e->bind.value = value;
                return e;
            }

            //Plain Identifier
            struct Expr* e = alloc_expr(p, expr_Ident, t->span);
            e->ident.string_id = t->string_id;
            e->ident.span = t->span;
            return e;
        }
        case LParen: {
            advance(p);
            struct Expr* inner = parse_expr_prec(p, PREC_NONE);
            expect(p, RParen);
            return inner;
        }
        case LBrace: {
            advance(p);
            struct Expr* e = alloc_expr(p, expr_Block, t->span);
            vec_init(&e->block.stmts, sizeof(struct Expr*));
            while (!check(p, RBrace) && !check(p, Eof)) {
                struct Expr* stmt = parse_expr_prec(p, PREC_NONE);
                if (stmt) vec_push(&e->block.stmts, &stmt);
                match(p, Semicolon);
            }
            expect(p, RBrace);
            return e;
        }
        default: {
            fprintf(stderr, "error: unexpected token %s (line %d col %d)\n",
                token_kind_to_str(t->kind), t->span.line, t->span.column);
            advance(p);  // skip to avoid infinite loop
            return NULL;
        }
    }
}

static struct Expr* parse_expr_prec(struct Parser* p, enum Precedence min_prec) {
    struct Expr* left = parse_primary(p);
    if (!left) return NULL;

    for (;;) {
        struct Token* t = peek(p);
        enum Precedence prec = get_precedence(t->kind);
        if (prec <= min_prec) break;

        enum TokenKind op = t->kind;
        advance(p);

        struct Expr* right = parse_expr_prec(p, prec);

        struct Expr* bin = alloc_expr(p, expr_Bin, left->span);
        bin->bin.op = op;
        bin->bin.Left = left;
        bin->bin.Right = right;
        left = bin;
    }

    return left;
}

Vec* parse(struct Parser* p) {
    Vec* stmts = malloc(sizeof(Vec));
    vec_init(stmts, sizeof(struct Expr*));

    while (!check(p, Eof)) {
        struct Expr* expr = parse_expr_prec(p, PREC_NONE);
        if (expr) {
            vec_push(stmts, &expr);
        }
        // Consume semicolon between top-level expressions
        match(p, Semicolon);
    }

    return stmts;
}