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
    expr_For,        // for .. in
    expr_Switch,     // switch expr
    expr_Block,      // { ... }
    expr_Product,    // .{ field = val, ... }
    expr_Bind,       // x := expr, x :: expr
    expr_Ctl,        // ctl(params) ret_type | body
    expr_With,       // with handler
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
    expr_DestructureBind
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

// -- For Expressions --

struct ForExpr {
    Vec* bindings;
    struct Expr* iter;
    struct Expr* where_clause;
    struct Expr* body;
};

struct BlockExpr {
    Vec stmts;
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
};

struct DestructureBindExpr {
    struct Expr* pattern;   // Ident or Product (of Idents/Patterns)
    struct Expr* value;
    bool is_const;
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
    struct Identifier scope_param;  // <s> — zero if none
    Vec* operations;                // Vec of Expr* (bind expressions with lambda signatures)
};

struct EffectRowExpr {
    struct Expr* head;              // NULL for <|e>, otherwise the closed effect expression before |
    struct Identifier row;          // effect-row variable after |
};

// -- With --

struct WithExpr {
    struct Expr* func;
    struct Expr* body; 
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

struct Param {
    struct Identifier name;
    struct Expr* type_ann; // Null if not typed
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
        struct ForExpr for_expr;
        struct BlockExpr block;
        struct ProductExpr product;
        struct BindExpr bind;
        struct CtlExpr ctl;
        struct StructExpr struct_expr;
        struct WithExpr with;
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

#endif // AST_H
