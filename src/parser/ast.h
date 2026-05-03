#ifndef AST_H
#define AST_H

#include <stdint.h>
#include <stdbool.h>

#include "../common/vec.h"
#include "../lexer/token.h"

struct Decl;

struct Expr;

// Node Id
struct NodeId {
    uint32_t id;
};

// Forward declarations from name resolution
struct Decl;

// Identifiers
struct Identifier {
    uint32_t string_id;
    struct Span span;
    struct Decl* resolved;     // back-pointer set by name resolution; NULL until resolved
};

// All expression kinds
enum ExprKind {
    expr_Lit,        // 42, "hello", true
    expr_Ident,      // foo
    expr_Bin,        // x + y, x <- y
    expr_Assign,     // x = y
    expr_Unary,      // &x, ~x
    expr_Call,       // foo(x, y) 
    expr_Builtin,    // @sizeof(t)
    expr_If,         // if/then/else
    expr_Switch,     // switch expr
    expr_Block,      // { ... }
    expr_Product,    // .{ field = val, ... }
    expr_Bind,       // x := expr, x :: expr
    expr_Ctl,        // ctl(params) ret_type | body
    expr_Handler,    // handler { op :: ... } / handle (target) { ... }
    expr_With,       // with [x :=] caller body — closure sugar
    expr_Field,      // x.name
    expr_Index,      // buf[i]
    expr_Lambda,     // |args| body
    expr_Loop,       // Loop cond body
    expr_Struct,     // struct
    expr_Enum,       // enum
    expr_EnumRef,     // enum reference
    expr_Effect,      // effect declaration
    expr_EffectRow,   // <Effect | e> / <|e> effect-row annotation payload
    expr_Asm,         // inline assembly
    expr_Return,      // return expr
    expr_Break,       // break
    expr_Continue,    // continue
    expr_Defer,       // defer expr
    expr_ArrayType,   // []T, [N]T, [^]T
    expr_ArrayLit,    // [N]T{...}
    expr_SliceType,   // []t
    expr_ManyPtrType,  // [^]
    expr_DestructureBind,
    expr_Wildcard,    // bare `_` — placeholder pattern, ignored binding
};

// Literal expression
enum LitKind {
    lit_Int,
    lit_Float,
    lit_String,
    lit_Byte,
    lit_True,
    lit_False,
    lit_Nil,
};

struct LitExpr {
    enum LitKind kind;
    uint32_t string_id;
};

// -- Binary Expressions --

struct BinExpr {
    enum TokenKind op; // Plus, Minus, Star, etc.
    struct Expr* Left;
    struct Expr* Right;
};

// -- Assignment Expression --
struct AssignExpr {
    struct Expr* target;
    struct Expr* value;
};


// -- Unary Expressions --

enum UnaryOp {
    unary_Ref,      // &x
    unary_Deref,    // x^ or *x
    unary_Neg,      // -x
    unary_Not,      // !x
    unary_BitNot,   // ~x
    unary_Const,    // const T
    unary_Optional, // ?T
    unary_Inc,      // x++
    unary_Ptr,      // ^T (pointer type)
    unary_ManyPtr,  // [^]T (many pointer type)
};

struct UnaryExpr {
    enum UnaryOp op;
    struct Expr* operand;
    bool postfix;
};

// -- Call Expressions --

struct CallExpr {
    struct Expr* callee;
    Vec* args;
};

struct BuiltinExpr {
    uint32_t name_id; // e.g. "sizeof", "ptrcast", etc.
    Vec* args;
};

// -- If Expressions --

struct IfExpr {
    struct Expr* condition;
    struct Expr* then_branch;
    struct Expr* else_branch; // can be NULL if no else
    struct Identifier capture; // optional unwrap: if x |capture| then
};

struct BlockExpr {
    Vec* stmts;
};

// -- Switch --

struct SwitchArm {
    Vec* patterns;  // Vec of Expr* — multiple patterns with | (or)
    struct Expr* body;
};

struct SwitchExpr {
    struct Expr* scrutinee;
    Vec* arms;
};

// -- Product Literal --

struct ProductField {
    struct Identifier name; // Null-ish if positional
    struct Expr* value; 
    bool is_spread;
};

struct ProductExpr {
    struct Expr* type_expr;  // NULL for .{}, non-NULL for Type.{}
    Vec* Fields;
};

// -- Bind --

enum BindKind {
    bind_Const, // ::
    bind_Var,   // :=
    bind_Typed, // x : T = expr, x : T : expr
};

struct BindExpr {
    enum BindKind kind;
    struct Identifier name;
    struct Expr* type_ann; // Null if not typed
    struct Expr* value;
    bool is_pub;          // `pub` prefix on a top-level decl. Reserved for the
                          // future "private by default" flip; today every
                          // top-level decl is exported regardless.
};

struct DestructureBindExpr {
    struct Expr* pattern;   // Ident or Product (of Idents/Patterns)
    struct Expr* value;
    bool is_const;
    bool is_pub;
};

// -- Enum --

struct EnumVariant {
    struct Identifier name;
    struct Expr* explicit_value;
    struct Span span;
};

struct EnumExpr {
    Vec* variants;
};

struct EnumRefExpr {
    struct Identifier name;
};

// -- Struct --

enum StructMemberKind {
    member_Field,
    member_Union,
};

struct FieldDef {
    struct Identifier name;
    struct Expr* type;                  // Expr* so it can be any type expr
    struct Expr* default_value;         // nullable
};

struct UnionDef {
    Vec* variants;                      // Vec of FieldDef: each variant is `name: type`
};

struct StructMember {
    enum StructMemberKind kind;
    struct Span span;
    union {
        struct FieldDef field;
        struct UnionDef union_def;
    };
};

struct StructExpr {
    Vec* members;     // Vec of StructMember
    Vec* type_params; // nullable, for generics
};

// -- Effect --

struct EffectExpr {
    bool is_named;
    bool is_scoped;
    Vec* operations;                // Vec of Expr* (bind expressions with lambda signatures)
};

struct EffectRowExpr {
    struct Expr* head;              // NULL for <|e>, otherwise the closed effect expression before |
    struct Identifier row;          // effect-row variable after |
};

// -- Field access --

struct FieldExpr {
    struct Expr* object;
    struct Identifier field;
};

// -- Index --

struct IndexExpr {
    struct Expr* object;
    struct Expr* index;
};

// -- Lambda --

// How a parameter is supplied to its function.
//   PARAM_RUNTIME          — runtime value, written explicitly at the call site.
//   PARAM_COMPTIME         — comptime value, written explicitly at the call site
//                            and folded by const_eval (e.g. `comptime t: type`).
//   PARAM_INFERRED_COMPTIME — comptime value the user does NOT write at the call
//                            site. Sema fills it from context. Today: scope
//                            tokens drawn from the active evidence vector.
//                            Reserved for future uses (type-class dictionaries,
//                            region tokens) so we don't need a fourth kind.
typedef enum {
    PARAM_RUNTIME,
    PARAM_COMPTIME,
    PARAM_INFERRED_COMPTIME,
} ParamKind;

struct Param {
    struct Identifier name;
    struct Expr* type_ann; // Null if not typed
    ParamKind kind;
};

struct LambdaExpr {
    Vec* params;
    struct Expr* effect;
    struct Expr* ret_type; // Null if not annoatated
    struct Expr* body;
};

// -- Ctl --
struct CtlExpr {
    Vec* params;
    struct Expr* ret_type; // Null if it's an implementation with a body
    struct Expr* body;     // Null if it's a signature with a return type
};

// -- Handler --
//
// A handler value: a set of operation implementations plus three
// optional lifecycle clauses. Two source forms produce this node:
//
//   handler { op :: fn(...) ...; ...; initially e; finally e; return(e) }
//   handle (target) { ... same body ... }
//
// `target` is set only by the `handle` form (the value being handled
// against). `initially_clause` / `finally_clause` / `return_clause`
// hold bare expressions; the surface forms are
//   `initially <expr>`, `finally <expr>`, `return(<expr>)`.
// The parser unwraps the `expr_Return` payload of `return(...)` into
// `return_clause` so all three lifecycle slots are uniform bare exprs.
struct HandlerOp {
    struct Identifier name;
    bool is_ctl;
    Vec* params;             // Vec<Param>
    struct Expr* ret_type;   // nullable
    struct Expr* body;
    struct Span span;
};

// -- With --
//
// `with` is closure sugar. The parser produces this node uniformly:
//   with body                              → binder=0, caller, body
//   with x := caller body                  → binder=x, caller, body
//   with handler { ops } body              → binder=0, caller=expr_Handler
//   with x := named handler {ops} body     → binder=x, caller=expr_Handler
//
// `binder.string_id == 0` means anonymous (no name introduced in body's
// scope). The parser rejects `with x := handler {…}` (no `named`) — to
// bind a handler value, the user must explicitly write `named`.
//
// Sema branches on `caller`'s shape: handler literal vs other. The
// `with` node itself doesn't know about effects — that's a property of
// what the caller is.
struct WithExpr {
    struct Identifier binder;
    struct Expr* caller;
    struct Expr* body;
};

struct HandlerExpr {
    struct Expr* target;             // nullable; set by `handle (target) { … }`
    Vec* operations;                 // Vec<HandlerOp*>
    struct Expr* initially_clause;   // nullable; bare expr after `initially`
    struct Expr* finally_clause;     // nullable; bare expr after `finally`
    struct Expr* return_clause;      // nullable; payload of `return(<expr>)`
    struct Decl* effect_decl;        // resolver fills (phase 2)
};

// -- Loop --

struct LoopExpr {
    struct Expr* init;       // nullable: the init clause (often a Bind)
    struct Expr* condition;  // nullable: the loop condition (NULL = infinite)
    struct Expr* step;       // nullable: the per-iteration step
    struct Expr* body;       // the loop body
    struct Identifier capture;  // for unwrap-style loops, if applicable
};

// -- Array Literal --
struct ArrayLitExpr {
    struct Expr* size;
    bool size_inferred;
    struct Expr* elem_type;
    struct Expr* initializer;
};

struct SliceExpr {
    struct Expr* elem;
};

struct ManyPtrType {
    struct Expr* elem;
};


// -- the Expr Node ---

struct Expr {
    enum ExprKind kind;
    struct Span span;
    bool is_comptime;
    union {
        struct LitExpr lit;
        struct Identifier ident;
        struct BinExpr bin;
        struct AssignExpr assign;
        struct UnaryExpr unary;
        struct CallExpr call;
        struct BuiltinExpr builtin;
        struct IfExpr if_expr;
        struct BlockExpr block;
        struct ProductExpr product;
        struct BindExpr bind;
        struct CtlExpr ctl;
        struct HandlerExpr handler;
        struct WithExpr with;
        struct StructExpr struct_expr;
        struct FieldExpr field;
        struct IndexExpr index;
        struct LambdaExpr lambda;
        struct SwitchExpr switch_expr;
        struct LoopExpr loop_expr;
        struct EnumExpr enum_expr;
        struct EnumVariant enum_variant_expr;
        struct EnumRefExpr enum_ref_expr;
        struct EffectExpr effect_expr;
        struct EffectRowExpr effect_row;
        struct { uint32_t string_id; } asm_expr;
        struct { struct Expr* value; } return_expr;
        struct { struct Expr* value; } defer_expr;
        struct { struct Expr* size; struct Expr* elem; bool is_many_ptr; } array_type;
        struct SliceExpr slice_type;
        struct ManyPtrType many_ptr_type;
        struct ArrayLitExpr array_lit;
        struct DestructureBindExpr destructure;
        // break and continue have no payload — just the kind + span
    };
};

// ----- Shared AST helpers -----
//
// `expr_Ident` and `expr_Field` are the two AST kinds whose `Identifier`
// gets resolved to a `Decl*` by name-resolution. Several walkers
// (resolver, effect_solver, effects) need to ask "what decl does this
// expression refer to?" — these inlined helpers consolidate that
// extraction so every caller doesn't reimplement the same if/else.
static inline struct Decl* ast_resolved_decl_of(struct Expr* expr) {
    if (!expr) return NULL;
    if (expr->kind == expr_Ident) return expr->ident.resolved;
    if (expr->kind == expr_Field) return expr->field.field.resolved;
    return NULL;
}

static inline uint32_t ast_name_id_of(struct Expr* expr) {
    if (!expr) return 0;
    if (expr->kind == expr_Ident) return expr->ident.string_id;
    if (expr->kind == expr_Field) return expr->field.field.string_id;
    return 0;
}

#endif // AST_H
