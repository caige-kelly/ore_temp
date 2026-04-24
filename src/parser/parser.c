#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "ast.h"
#include "../lexer/token.h"
#include "common/arena.h"
#include "common/vec.h"
#include "./parser.h"

// -- Print --

static void print_indent(int indent) {
    for (int i = 0; i < indent; i++) printf("  ");
}

void print_ast(struct Expr* expr, StringPool* pool, int indent) {
    if (!expr) { print_indent(indent); printf("NULL\n"); return; }
    
    print_indent(indent);
    switch (expr->kind) {
        case expr_Lit:
            printf("Lit: \"%s\"\n", pool_get(pool, expr->lit.string_id, 0));
            break;
        case expr_Ident:
            printf("Ident: \"%s\"\n", pool_get(pool, expr->ident.string_id, 0));
            break;
        case expr_Bin:
            printf("Bin: %s\n", token_kind_to_str(expr->bin.op));
            print_ast(expr->bin.Left, pool, indent + 1);
            print_ast(expr->bin.Right, pool, indent + 1);
            break;
        case expr_Bind:
            printf("Bind (%s): \"%s\"\n",
                expr->bind.kind == bind_Const ? "::" : ":=",
                pool_get(pool, expr->bind.name.string_id, 0));
            if (expr->bind.type_ann) {
                print_indent(indent + 1); printf("type:\n");
                print_ast(expr->bind.type_ann, pool, indent + 2);
            }
            print_indent(indent + 1); printf("value:\n");
            print_ast(expr->bind.value, pool, indent + 2);
            break;
        case expr_Block:
            printf("Block:\n");
            for (size_t i = 0; i < expr->block.stmts.count; i++) {
                struct Expr** e = (struct Expr**)vec_get(&expr->block.stmts, i);
                if (e) print_ast(*e, pool, indent + 1);
            }
            break;
        case expr_With:
            printf("With:\n");
            print_indent(indent + 1); printf("func:\n");
            print_ast(expr->with.func, pool, indent + 2);
            if (expr->with.body) {
                print_indent(indent + 1); printf("body:\n");
                print_ast(expr->with.body, pool, indent + 2);
            }
            break;
        case expr_Product:
            printf("Product:\n");
            for (size_t i = 0; i < expr->product.Fields->count; i++) {
                struct ProductField* f = (struct ProductField*)vec_get(expr->product.Fields, i);
                if (f) {
                    if (f->name.string_id) {
                        print_indent(indent + 1);
                        printf(".%s =\n", pool_get(pool, f->name.string_id, 0));
                        print_ast(f->value, pool, indent + 2);
                    } else {
                        print_ast(f->value, pool, indent + 1);
                    }
                }
            }
            break;
        case expr_While:
            printf("While:\n");
            print_indent(indent + 1); printf("cond:\n");
            print_ast(expr->while_expr.condition, pool, indent + 2);
            print_indent(indent + 1); printf("body:\n");
            print_ast(expr->while_expr.body, pool, indent + 2);
            break;
        case expr_For:
            printf("For:\n");
            print_indent(indent + 1); printf("bindings:\n");
            for (size_t i = 0; i < expr->for_expr.bindings->count; i++) {
                struct Param* b = (struct Param*)vec_get(expr->for_expr.bindings, i);
                if (b) {
                    print_indent(indent + 2);
                    printf("%s", pool_get(pool, b->name.string_id, 0));
                    if (b->type_ann) {
                        printf(": ");
                        print_ast(b->type_ann, pool, 0);
                    } else {
                        printf("\n");
                    }
                }
            }
            print_indent(indent + 1); printf("iter:\n");
            print_ast(expr->for_expr.iter, pool, indent + 2);
            if (expr->for_expr.where_clause) {
                print_indent(indent + 1); printf("where:\n");
                print_ast(expr->for_expr.where_clause, pool, indent + 2);
            }
            print_indent(indent + 1); printf("body:\n");
            print_ast(expr->for_expr.body, pool, indent + 2);
            break;
        case expr_Builtin:
            printf("Builtin: @%s\n", pool_get(pool, expr->builtin.name_id, 0));
            if (expr->builtin.args) {
                for (size_t i = 0; i < expr->builtin.args->count; i++) {
                    struct Expr** arg = (struct Expr**)vec_get(expr->builtin.args, i);
                    if (arg) print_ast(*arg, pool, indent + 1);
                }
            }
            break;
        case expr_Field:
            printf("Field: .%s\n", pool_get(pool, expr->field.field.string_id, 0));
            print_ast(expr->field.object, pool, indent + 1);
            break;
        case expr_Index:
            printf("Index:\n");
            print_indent(indent + 1); printf("object:\n");
            print_ast(expr->index.object, pool, indent + 2);
            print_indent(indent + 1); printf("index:\n");
            print_ast(expr->index.index, pool, indent + 2);
            break;
        case expr_Call:
            printf("Call:\n");
            print_indent(indent + 1); printf("callee:\n");
            print_ast(expr->call.callee, pool, indent + 2);
            print_indent(indent + 1); printf("args:\n");
            for (size_t i = 0; i < expr->call.args->count; i++) {
                struct Expr** arg = (struct Expr**)vec_get(expr->call.args, i);
                if (arg) print_ast(*arg, pool, indent + 2);
            }
            break;
        case expr_If:
            printf("If:\n");
            print_indent(indent + 1); printf("cond:\n");
            print_ast(expr->if_expr.condition, pool, indent + 2);
            print_indent(indent + 1); printf("then:\n");
            print_ast(expr->if_expr.then_branch, pool, indent + 2);
            if (expr->if_expr.else_branch) {
                print_indent(indent + 1); printf("else:\n");
                print_ast(expr->if_expr.else_branch, pool, indent + 2);
            }
            break;
        case expr_Unary: {
            const char* ops[] = { "&", "*", "-", "!", "~", "const", "?", "++" };
            printf("Unary: %s%s\n", expr->unary.postfix ? "postfix " : "", ops[expr->unary.op]);
            print_ast(expr->unary.operand, pool, indent + 1);
            break;
        }
        case expr_Lambda:
            printf("Lambda:\n");
            print_indent(indent + 1); printf("params:\n");
            for (size_t i = 0; i < expr->lambda.params->count; i++) {
                struct Param* param = (struct Param*)vec_get(expr->lambda.params, i);
                if (param) {
                    print_indent(indent + 2);
                    printf("%s", pool_get(pool, param->name.string_id, 0));
                    if (param->type_ann) {
                        printf(": ");
                        print_ast(param->type_ann, pool, 0);
                    } else {
                        printf("\n");
                    }
                }
            }
            if (expr->lambda.ret_type) {
                print_indent(indent + 1); printf("returns:\n");
                print_ast(expr->lambda.ret_type, pool, indent + 2);
            }
            if (expr->lambda.effect) {
                print_indent(indent + 1); printf("effect:\n");
                print_ast(expr->lambda.effect, pool, indent + 2);
            }
            print_indent(indent + 1); printf("body:\n");
            print_ast(expr->lambda.body, pool, indent + 2);
            break;

        case expr_Struct:
            printf("Struct:\n");
            print_indent(indent + 1); printf("members:\n");
            for (size_t i = 0; i < expr->struct_expr.members->count; i++) {
                struct StructMember* m = (struct StructMember*)vec_get(expr->struct_expr.members, i);
                if (!m) continue;
                
                if (m->kind == member_Field) {
                    print_indent(indent + 2);
                    printf("Field: %s\n", pool_get(pool, m->field.name.string_id, 0));
                    print_indent(indent + 3); printf("type:\n");
                    print_ast(m->field.type, pool, indent + 4);
                } else {
                    print_indent(indent + 2); printf("Union:\n");
                    print_indent(indent + 3); printf("variants:\n");
                    for (size_t j = 0; j < m->union_def.variants->count; j++) {
                        struct FieldDef* v = (struct FieldDef*)vec_get(m->union_def.variants, j);
                        if (!v) continue;
                        print_indent(indent + 4);
                        printf("Variant: %s\n", pool_get(pool, v->name.string_id, 0));
                        print_indent(indent + 5); printf("type:\n");
                        print_ast(v->type, pool, indent + 6);
                    }
                }
            }
            break;
        
        case expr_Enum:
            printf("Enum:\n");
            print_indent(indent + 1); printf("variants:\n");
            for (size_t i = 0; i < expr->enum_expr.variants->count; i++) {
                struct EnumVariant* v = (struct EnumVariant*)vec_get(expr->enum_expr.variants, i);
                if (!v) continue;
                print_indent(indent + 2);
                printf("Variant: %s", pool_get(pool, v->name.string_id, 0));
                if (v->explicit_value) {
                    printf(" = \n");
                    print_ast(v->explicit_value, pool, indent + 3);
                } else {
                    printf("\n");
                }
            }
            break;

        case expr_Asm:
            printf("Asm: \"%s\"\n", pool_get(pool, expr->asm_expr.string_id, 0));
            break;
        case expr_Effect:
            printf("Effect:%s%s\n",
                expr->effect_expr.is_named ? " named" : "",
                expr->effect_expr.is_scoped ? " scoped" : "");
            if (expr->effect_expr.scope_param.string_id) {
                print_indent(indent + 1);
                printf("scope: <%s>\n", pool_get(pool, expr->effect_expr.scope_param.string_id, 0));
            }
            print_indent(indent + 1); printf("operations:\n");
            for (size_t i = 0; i < expr->effect_expr.operations->count; i++) {
                struct Expr** op = (struct Expr**)vec_get(expr->effect_expr.operations, i);
                if (op) print_ast(*op, pool, indent + 2);
            }
            break;
        case expr_EnumRef:
            printf("EnumRef: %s\n", pool_get(pool, expr->enum_ref_expr.name.string_id, 0));
            break;
        
        case expr_Switch:
            printf("Switch:\n");
            print_indent(indent + 1); printf("scrutinee:\n");
            print_ast(expr->switch_expr.scrutinee, pool, indent + 2);
            print_indent(indent + 1); printf("arms:\n");
            for (size_t i = 0; i < expr->switch_expr.arms->count; i++) {
                struct SwitchArm* arm = (struct SwitchArm*)vec_get(expr->switch_expr.arms, i);
                if (!arm) continue;
                print_indent(indent + 2); printf("arm:\n");
                print_indent(indent + 3); printf("patterns:\n");
                for (size_t j = 0; j < arm->patterns->count; j++) {
                    struct Expr** pat = (struct Expr**)vec_get(arm->patterns, j);
                    if (pat) print_ast(*pat, pool, indent + 4);
                }
                print_indent(indent + 3); printf("body:\n");
                print_ast(arm->body, pool, indent + 4);
            }
            break;
          

        default:
            printf("<%s>\n", "TODO");
            break;
    }
}

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
        case OrElse: return PREC_OR;
        case LeftArrow:
        case PlusEqual: case MinusEqual: case StarEqual:
        case ForwardSlashEqual: case PercentEqual:
        case PipeEqual: case AmpersandEqual: case CaretEqual:
            return PREC_ASSIGN;
        case PipePipe: return PREC_OR;
        case AmpersandAmpersand: return PREC_AND;
        case EqualEqual: case BangEqual: return PREC_EQUALITY;
        case Less: case Greater: case LessEqual: case GreaterEqual: return PREC_COMPARISON;
        case DotDot: return PREC_RANGE;
        case Pipe: case Ampersand: case Caret: return PREC_BITWISE;
        case ShiftLeft: case ShiftRight: return PREC_SHIFT;
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

static bool check_at(struct Parser* p, size_t offset, enum TokenKind kind) {
    struct Token* t = (struct Token*)vec_get(p->tokens, p->current + offset);
    return t && t->kind == kind;
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

// -- Error Recovery --

static void synchronize(struct Parser* p) {
    while (!check(p, Eof)) {
        // Stop at statement boundaries
        if (check(p, Semicolon)) { advance(p); return; }
        if (check(p, RBrace)) return;

        // Stop at tokens that start new statements
        struct Token* t = peek(p);
        if (t) {
            switch (t->kind) {
                case If: case For: case Switch:
                case With: case Break: case Continue:
                    return;
                default: break;
            }
        }

        advance(p);
    }
}

// -- Parse Functions --

static struct Expr* parse_expr_prec(struct Parser* p, enum Precedence min_prec);

// Parses: name1 :: type1; name2 :: type2; ...
static Vec* parse_variant_list(struct Parser* p) {
    Vec* variants = vec_new_in(p->arena, sizeof(struct FieldDef));

    while (!check(p, RBrace) && !check(p, Eof)) {
        size_t pos_before = p->current;

        struct Token* name = expect(p, Identifier);
        if (!name) break;

        expect(p, ColonColon);
        struct Expr* type = parse_expr_prec(p, PREC_BITWISE);

        struct FieldDef variant = {
            .name = (struct Identifier){ .string_id = name->string_id, .span = name->span },
            .type = type,
        };
        vec_push(variants, &variant);

        match(p, Semicolon);

        if (p->current == pos_before) advance(p);
    }

    return variants;
}

// Parse block statements, handling 'with' nesting recursively
static struct Expr* parse_block_stmts(struct Parser* p, struct Span span) {
    struct Expr* e = alloc_expr(p, expr_Block, span);
    vec_init(&e->block.stmts, sizeof(struct Expr*));

    while (!check(p, RBrace) && !check(p, Eof)) {
        struct Expr* stmt = parse_expr_prec(p, PREC_NONE);
        if (!stmt) { match(p, Semicolon); continue; }

        if (stmt->kind == expr_With) {
            match(p, Semicolon);
            // Recursively parse remaining stmts as the with body
            stmt->with.body = parse_block_stmts(p, stmt->span);
            vec_push(&e->block.stmts, &stmt);
            break;  // with consumed rest of block
        }

        vec_push(&e->block.stmts, &stmt);
        match(p, Semicolon);
    }

    return e;
}

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
        case AsmLit: {
            advance(p);
            struct Expr* e = alloc_expr(p, expr_Asm, t->span);
            e->asm_expr.string_id = t->string_id;
            return e;
        }
        case Nil:
        case Void:
        case NoReturn:
        case AnyType:
        case Underscore:
        case Type: {
            advance(p);
            struct Expr* e = alloc_expr(p, expr_Ident, t->span);
            e->ident.string_id = t->string_id;
            e->ident.span = t->span;
            return e;
        }
        case Identifier: {
            advance(p);
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
            struct Expr* e = parse_block_stmts(p, t->span);
            expect(p, RBrace);
            return e;
        }
        case Pipe: {
            advance(p);  // consume opening |

            Vec* params = vec_new_in(p->arena, sizeof(struct Param));

            // Parse params: |x: i32, y: i32| or |x, y| or |void|
            // |void| means zero params
            if (check(p, Void)) {
                advance(p);  // consume void
            } else if (!check(p, Pipe)) {
                for (;;) {
                    // Skip comptime modifier
                    match(p, Comptime);

                    struct Token* name = expect(p, Identifier);
                    if (!name) break;

                    struct Param param = {
                        .name = { .string_id = name->string_id, .span = name->span },
                        .type_ann = NULL,
                    };

                    // Optional type annotation: x: i32
                    // Parse at PREC_BITWISE so | is not consumed as binary op
                    if (match(p, Colon)) {
                        param.type_ann = parse_expr_prec(p, PREC_BITWISE);
                    }

                    vec_push(params, &param);

                    if (!match(p, Comma)) break;
                }
            }

            expect(p, Pipe);  // consume closing |

            // Optional return type after ->
            struct Expr* ret_type = NULL;
            if (match(p, RightArrow)) {
                ret_type = parse_expr_prec(p, PREC_BITWISE);
            }

            // Optional effect annotation <Exn> or <Exn, IO>
            struct Expr* effect = NULL;
            if (match(p, Less)) {
                effect = parse_expr_prec(p, PREC_COMPARISON);
                expect(p, Greater);
            }

            // Parse body — unless next token suggests we're a type signature
            // (no body: followed by |, ), comma, >, ;, RBrace)
            struct Expr* body = NULL;
            struct Token* next_tok = peek(p);
            if (next_tok && next_tok->kind != Pipe && next_tok->kind != RParen &&
                next_tok->kind != Comma && next_tok->kind != Greater &&
                next_tok->kind != Semicolon && next_tok->kind != RBrace &&
                next_tok->kind != Eof) {
                body = parse_expr_prec(p, PREC_NONE);
            }

            struct Expr* e = alloc_expr(p, expr_Lambda, t->span);
            e->lambda.params = params;
            e->lambda.effect = effect;
            e->lambda.ret_type = ret_type;
            e->lambda.body = body;
            return e;
        }

        // With: multiple forms, all start with 'with'
        // with exn, console, malloc
        // with handler { ... }
        // with a : arenaAllocator = arena(4096)
        // with [1,2,3].foreach
        case With: {
            advance(p);  // consume with

            struct Expr* func = parse_expr_prec(p, PREC_NONE);

            struct Expr* e = alloc_expr(p, expr_With, t->span);
            e->with.func = func;
            e->with.body = NULL;  // block parser fills this
            return e;
        }

        // If/then/else/elif
        case If: case Elif: {
            advance(p);  // consume if or elif
            struct Expr* condition = parse_expr_prec(p, PREC_NONE);
            expect(p, Then);
            struct Expr* then_branch = parse_expr_prec(p, PREC_NONE);

            struct Expr* else_branch = NULL;
            if (check(p, Elif)) {
                // elif is just another if — recurse
                else_branch = parse_primary(p);
            } else if (match(p, Else)) {
                else_branch = parse_expr_prec(p, PREC_NONE);
            }

            struct Expr* e = alloc_expr(p, expr_If, t->span);
            e->if_expr.condition = condition;
            e->if_expr.then_branch = then_branch;
            e->if_expr.else_branch = else_branch;
            return e;
        }

        // Array/slice types: [26; f64], []u8, [*]u8
        case LBracket: {
            advance(p);  // consume [

            // []T — slice type
            if (check(p, RBracket)) {
                advance(p);
                struct Expr* elem = parse_expr_prec(p, PREC_UNARY);
                struct Expr* e = alloc_expr(p, expr_Index, t->span);
                e->index.object = NULL;  // NULL object signals slice type
                e->index.index = elem;
                return e;
            }

            // [*]T — tracked pointer type
            if (check(p, Star)) {
                advance(p);
                expect(p, RBracket);
                struct Expr* elem = parse_expr_prec(p, PREC_UNARY);
                struct Expr* e = alloc_expr(p, expr_Unary, t->span);
                e->unary.op = unary_Deref;  // reuse deref to mean tracked ptr
                e->unary.operand = elem;
                e->unary.postfix = false;
                return e;
            }

            // [N; T] — fixed array type, or [expr] — array literal
            struct Expr* first = parse_expr_prec(p, PREC_NONE);
            if (match(p, Semicolon)) {
                // [N; T] — fixed array
                struct Expr* elem_type = parse_expr_prec(p, PREC_NONE);
                expect(p, RBracket);
                struct Expr* e = alloc_expr(p, expr_Index, t->span);
                e->index.object = elem_type;   // element type
                e->index.index = first;         // size
                return e;
            }

            // Just [expr] — bracket expression
            expect(p, RBracket);
            return first;
        }

        // Product literal: .{ x, y } or .{ .name = val, .age = val }
        case Dot: {
            if (peek(p) && p->current + 1 < p->tokens->count) {
                struct Token* next = (struct Token*)vec_get(p->tokens, p->current + 1);
                if (next && next->kind == LBrace) {
                    advance(p);  // consume .
                    advance(p);  // consume {

                    struct Expr* e = alloc_expr(p, expr_Product, t->span);
                    Vec* fields = vec_new_in(p->arena, sizeof(struct ProductField));

                    if (!check(p, RBrace)) {
                        for (;;) {
                            struct ProductField field = { .name = {0}, .value = NULL };

                            // Named field: .name = expr
                            if (check(p, Dot)) {
                                advance(p);  // consume .
                                struct Token* fname = expect(p, Identifier);
                                if (fname) {
                                    field.name = (struct Identifier){ .string_id = fname->string_id, .span = fname->span };
                                }
                                expect(p, Equal);
                            }

                            field.value = parse_expr_prec(p, PREC_NONE);
                            vec_push(fields, &field);

                            if (!match(p, Comma)) break;
                        }
                    }
                    expect(p, RBrace);
                    e->product.Fields = fields;
                    return e;
                } else if (next && next->kind == Identifier) {
                    advance(p); // consomue '.'
                    struct Token* refname = expect(p, Identifier);
                    if (!refname) return NULL;

                    struct Expr* e = alloc_expr(p, expr_EnumRef, t->span);
                    e->enum_ref_expr.name = (struct Identifier){ 
                        .string_id=refname->string_id, 
                        .span=refname->span 
                    };
                    return e;
                }
            }
            // Plain dot — fall through to default error
            advance(p);
            fprintf(stderr, "error: unexpected '.' (line %d col %d)\n", t->span.line, t->span.column);
            synchronize(p);
            return NULL;
        }

        case Struct: {
            advance(p);  // consume struct
            
            Vec* members = vec_new_in(p->arena, sizeof(struct StructMember));

            expect(p, LBrace);

            while (!check(p, RBrace) && !check(p, Eof)) {
                size_t pos_before = p->current;
                struct StructMember member;

                if (match(p, Union)) {
                    expect(p, LBrace);
                    member.kind = member_Union;
                    member.span = peek(p)->span;
                    member.union_def.variants = parse_variant_list(p);
                    expect(p, RBrace);
                } else {
                    struct Token* name = expect(p, Identifier);
                    if (!name) break;
                    expect(p, Colon);
                    struct Expr* type = parse_expr_prec(p, PREC_BITWISE);
                    
                    member.kind = member_Field;
                    member.span = name->span;
                    member.field.name = (struct Identifier){ .string_id=name->string_id, .span=name->span};
                    member.field.type = type;
                }

                vec_push(members, &member);
                match(p, Semicolon);

                if (p->current == pos_before) advance(p);
            }

            expect(p, RBrace);

            struct Expr* e = alloc_expr(p, expr_Struct, t->span);
            e->struct_expr.members = members;
            return e;
        }

        case Enum: {
            advance(p); //consume 'enum'
            expect(p, LBrace);

            Vec* variants = vec_new_in(p->arena, sizeof(struct EnumVariant));

            while (!check(p, RBrace) && !check(p, Eof)) {

                size_t pos_before = p->current;

                struct Token* name = expect(p, Identifier);
                if (!name) break;

                struct Expr* explicit_value = NULL;
                if (match(p, Colon)) {
                    explicit_value = parse_expr_prec(p, PREC_BITWISE);
                }

                struct EnumVariant variant = {
                    .name = (struct Identifier){ .string_id = name->string_id, .span = name->span },
                    .explicit_value = explicit_value,
                    .span = name->span,
                };

                vec_push(variants, &variant);

                match(p, Semicolon);

                if (p->current == pos_before) advance(p);
            }

            expect(p, RBrace);

            struct Expr* e = alloc_expr(p, expr_Enum, t->span);
            e->enum_expr.variants = variants;
            return e;

        }

        case Switch: {
            advance(p);  // consume switch
            struct Expr* scrutinee = parse_expr_prec(p, PREC_NONE);

            expect(p, LBrace);

            Vec* arms = vec_new_in(p->arena, sizeof(struct SwitchArm));

            while(!check(p, RBrace) && !check(p, Eof)) {
                size_t pos_before = p->current;

                struct SwitchArm arm = { .patterns = NULL, .body = NULL };
                Vec* patterns = vec_new_in(p->arena, sizeof(struct Expr*));

                // Parse patterns separated by | (or)
                for (;;) {
                    struct Expr* pat = parse_primary(p);
                    if (pat) vec_push(patterns, &pat);
                    if (!match(p, Pipe)) break;
                }
                arm.patterns = patterns;
                expect(p, FatArrow);
                arm.body = parse_expr_prec(p, PREC_NONE);
                match(p, Semicolon);
                vec_push(arms, &arm);

                if (p->current == pos_before) advance(p);
            }

            struct Expr* e = alloc_expr(p, expr_Switch, t->span);
            e->switch_expr.scrutinee = scrutinee;
            e->switch_expr.arms = arms;

            expect(p, RBrace);

            return e;
        }

        // Handle: handle (expr) { handler defs }
        case Handle: {
            advance(p);  // consume handle
            struct Expr* func = parse_expr_prec(p, PREC_NONE);
            struct Expr* body = parse_expr_prec(p, PREC_NONE);

            struct Expr* e = alloc_expr(p, expr_With, t->span);
            e->with.func = func;
            e->with.body = body;
            return e;
        }

        // Handler block: handler { op :: |...| body; ... }
        case Handler: {
            advance(p);  // consume handler
            // Parse body as a block — operation definitions inside
            struct Expr* body = parse_expr_prec(p, PREC_NONE);
            return body;  // handler is just sugar — the block contains the defs
        }

        // Effect declaration: effect, named effect, scoped effect, named scoped effect<s>
        case Effect: case Named: case Scoped: {
            bool is_named = false;
            bool is_scoped = false;

            // Consume modifiers
            if (t->kind == Named) { is_named = true; advance(p); }
            if (check(p, Scoped)) { is_scoped = true; advance(p); }
            if (t->kind == Scoped && !is_named) { is_scoped = true; }

            if (!check(p, Effect)) {
                // Named/Scoped must be followed by effect
                fprintf(stderr, "error: expected 'effect' after named/scoped (line %d col %d)\n",
                    t->span.line, t->span.column);
                synchronize(p);
                return NULL;
            }
            advance(p);  // consume effect

            // Optional scope parameter <s>
            struct Identifier scope_param = {0};
            if (match(p, Less)) {
                struct Token* sp = expect(p, Identifier);
                if (sp) {
                    scope_param = (struct Identifier){ .string_id = sp->string_id, .span = sp->span };
                }
                expect(p, Greater);
            }

            // Parse operations block
            expect(p, LBrace);
            Vec* operations = vec_new_in(p->arena, sizeof(struct Expr*));

            while (!check(p, RBrace) && !check(p, Eof)) {
                size_t pos_before = p->current;
                struct Expr* op = parse_expr_prec(p, PREC_NONE);
                if (op) vec_push(operations, &op);
                match(p, Semicolon);
                if (p->current == pos_before) advance(p);
            }
            expect(p, RBrace);

            struct Expr* e = alloc_expr(p, expr_Effect, t->span);
            e->effect_expr.is_named = is_named;
            e->effect_expr.is_scoped = is_scoped;
            e->effect_expr.scope_param = scope_param;
            e->effect_expr.operations = operations;
            return e;
        }

        // While loop: while cond body
        case While: {
            advance(p);  // consume while
            struct Expr* condition = parse_expr_prec(p, PREC_NONE);
            struct Expr* body = parse_expr_prec(p, PREC_NONE);

            struct Expr* e = alloc_expr(p, expr_While, t->span);
            e->while_expr.condition = condition;
            e->while_expr.body = body;
            return e;
        }

        // For loops: for x: T in expr, for x: T, i: T in expr where cond
        case For: {
            advance(p);  // consume for

            // Parse bindings: x: T or x: T, i: T
            Vec* bindings = vec_new_in(p->arena, sizeof(struct Param));

            for (;;) {
                struct Token* name = expect(p, Identifier);
                if (!name) break;

                struct Param binding = {
                    .name = { .string_id = name->string_id, .span = name->span },
                    .type_ann = NULL,
                };

                if (match(p, Colon)) {
                    binding.type_ann = parse_expr_prec(p, PREC_BITWISE);
                }

                vec_push(bindings, &binding);

                if (!match(p, Comma)) break;
            }

            expect(p, In);

            struct Expr* iter = parse_expr_prec(p, PREC_NONE);

            // Optional where clause
            struct Expr* where_clause = NULL;
            if (match(p, Where)) {
                where_clause = parse_expr_prec(p, PREC_NONE);
            }

            // Parse body
            struct Expr* body = parse_expr_prec(p, PREC_NONE);

            struct Expr* e = alloc_expr(p, expr_For, t->span);
            e->for_expr.bindings = bindings;
            e->for_expr.iter = iter;
            e->for_expr.where_clause = where_clause;
            e->for_expr.body = body;
            return e;
        }

        // Builtins: @sizeOf(T), @ptrcast(x), @null, etc.
        case At: {
            advance(p);  // consume @
            struct Token* name = expect(p, Identifier);
            if (!name) return NULL;

            struct Expr* e = alloc_expr(p, expr_Builtin, t->span);
            e->builtin.name_id = name->string_id;

            // Some builtins have no args (like @null)
            if (check(p, LParen)) {
                advance(p);
                Vec* args = vec_new_in(p->arena, sizeof(struct Expr*));

                if (!check(p, RParen)) {
                    for (;;) {
                        struct Expr* arg = parse_expr_prec(p, PREC_NONE);
                        if (arg) vec_push(args, &arg);
                        if (!match(p, Comma)) break;
                    }
                }
                expect(p, RParen);
                e->builtin.args = args;
            } else {
                e->builtin.args = NULL;
            }
            return e;
        }

        // Prefix unary operators: *T, &x, -x, !x, ~x, const T
        case Star: case Ampersand: case Minus: case Bang: case Tilde: case Const: case Question: {
            advance(p);
            enum UnaryOp op;
            switch (t->kind) {
                case Star: op = unary_Deref; break;
                case Ampersand: op = unary_Ref; break;
                case Minus: op = unary_Neg; break;
                case Bang: op = unary_Not; break;
                case Tilde: op = unary_BitNot; break;
                case Const: op = unary_Const; break;
                case Question: op = unary_Optional; break;
                default: op = unary_Not; break;
            }
            struct Expr* operand = parse_expr_prec(p, PREC_UNARY);
            struct Expr* e = alloc_expr(p, expr_Unary, t->span);
            e->unary.op = op;
            e->unary.operand = operand;
            e->unary.postfix = false;
            return e;
        }
        default: {
            fprintf(stderr, "error: unexpected token %s (line %d col %d)\n",
                token_kind_to_str(t->kind), t->span.line, t->span.column);
            synchronize(p);
            return NULL;
        }
    }
}

static struct Expr* parse_expr_prec(struct Parser* p, enum Precedence min_prec) {
    struct Expr* left = parse_primary(p);
    if (!left) return NULL;

    for (;;) {
        struct Token* t = peek(p);
        if (!t) break;

        // Postfix: function call foo(x, y)
        if (t->kind == LParen && min_prec < PREC_POSTFIX) {
            advance(p);  // consume (
            Vec* args = vec_new_in(p->arena, sizeof(struct Expr*));

            if (!check(p, RParen)) {
                for (;;) {
                    struct Expr* arg = parse_expr_prec(p, PREC_NONE);
                    if (arg) vec_push(args, &arg);
                    if (!match(p, Comma)) break;
                }
            }
            expect(p, RParen);

            struct Expr* call = alloc_expr(p, expr_Call, left->span);
            call->call.callee = left;
            call->call.args = args;
            left = call;
            continue;
        }

        // Postfix: field access x.field, x->field, or x.0 (tuple)
        if ((t->kind == Dot || t->kind == RightArrow) && min_prec < PREC_POSTFIX) {
            advance(p);  // consume . or ->
            struct Token* field_name = peek(p);
            if (!field_name || (field_name->kind != Identifier && field_name->kind != IntLit)) {
                fprintf(stderr, "error: expected field name after '.' (line %d col %d)\n",
                    field_name ? field_name->span.line : 0, field_name ? field_name->span.column : 0);
                break;
            }
            advance(p);

            struct Expr* field = alloc_expr(p, expr_Field, left->span);
            field->field.object = left;
            field->field.field = (struct Identifier){ .string_id = field_name->string_id, .span = field_name->span };
            left = field;
            continue;
        }

        // Postfix: index x[i] or slice x[0..n]
        if (t->kind == LBracket && min_prec < PREC_POSTFIX) {
            advance(p);  // consume [
            struct Expr* index = parse_expr_prec(p, PREC_NONE);
            expect(p, RBracket);

            struct Expr* idx = alloc_expr(p, expr_Index, left->span);
            idx->index.object = left;
            idx->index.index = index;
            left = idx;
            continue;
        }

        // Postfix: x++ (increment)
        if (t->kind == PlusPlus && min_prec < PREC_POSTFIX) {
            advance(p);
            struct Expr* e = alloc_expr(p, expr_Unary, left->span);
            e->unary.op = unary_Inc;
            e->unary.operand = left;
            e->unary.postfix = true;
            left = e;
            continue;
        }

        // Bind: x :: expr, x := expr, x : T = expr, x : T : expr
        // Only at top-level expression (PREC_NONE) and only after an identifier
        if ((t->kind == ColonColon || t->kind == ColonEqual || t->kind == Colon)
            && left->kind == expr_Ident && min_prec == PREC_NONE) {

            if (t->kind == ColonColon) {
                advance(p);
                struct Expr* value = parse_expr_prec(p, PREC_NONE);
                struct Expr* e = alloc_expr(p, expr_Bind, left->span);
                e->bind.kind = bind_Const;
                e->bind.name = (struct Identifier){ .string_id = left->ident.string_id, .span = left->span };
                e->bind.type_ann = NULL;
                e->bind.value = value;
                left = e;
                break;
            }

            if (t->kind == ColonEqual) {
                advance(p);
                struct Expr* value = parse_expr_prec(p, PREC_NONE);
                struct Expr* e = alloc_expr(p, expr_Bind, left->span);
                e->bind.kind = bind_Var;
                e->bind.name = (struct Identifier){ .string_id = left->ident.string_id, .span = left->span };
                e->bind.type_ann = NULL;
                e->bind.value = value;
                left = e;
                break;
            }

            if (t->kind == Colon) {
                advance(p);
                // Parse type at PREC_ASSIGN — prevents recursive bind check
                struct Expr* type = parse_expr_prec(p, PREC_ASSIGN);

                enum BindKind kind;
                if (match(p, Equal)) {
                    kind = bind_Var;
                } else if (match(p, Colon)) {
                    kind = bind_Const;
                } else {
                    fprintf(stderr, "error: expected '=' or ':' after type annotation (line %d col %d)\n",
                        t->span.line, t->span.column);
                    break;
                }

                struct Expr* value = parse_expr_prec(p, PREC_NONE);
                struct Expr* e = alloc_expr(p, expr_Bind, left->span);
                e->bind.kind = kind;
                e->bind.name = (struct Identifier){ .string_id = left->ident.string_id, .span = left->span };
                e->bind.type_ann = type;
                e->bind.value = value;
                left = e;
                break;
            }
        }

        // Binary operators
        enum Precedence prec = get_precedence(t->kind);
        if (prec <= min_prec) break;

        enum TokenKind op = t->kind;
        advance(p);

        // Right-associative operators: <-, +=, -=, *=, /=, %=, **
        bool right_assoc = (op == LeftArrow || op == PlusEqual || op == MinusEqual ||
                            op == StarEqual || op == ForwardSlashEqual || op == PercentEqual ||
                            op == PipeEqual || op == AmpersandEqual || op == CaretEqual ||
                            op == StarStar);
        struct Expr* right = parse_expr_prec(p, right_assoc ? prec - 1 : prec);

        struct Expr* bin = alloc_expr(p, expr_Bin, left->span);
        bin->bin.op = op;
        bin->bin.Left = left;
        bin->bin.Right = right;
        left = bin;
    }

    return left;
}

Vec* parse(struct Parser* p) {
    Vec* stmts = vec_new_in(p->arena, sizeof(struct Expr*));

    while (!check(p, Eof)) {
        size_t pos_before = p->current;
        struct Expr* expr = parse_expr_prec(p, PREC_NONE);
        if (expr) {
            vec_push(stmts, &expr);
        }
        // Consume semicolon between top-level expressions
        match(p, Semicolon);
        // Safety: if no progress was made, skip token to prevent infinite loop
        if (p->current == pos_before) advance(p);
    }

    return stmts;
}