#ifndef AST_H
#define AST_H

#include <stdint.h>
#include <stdbool.h>

#include "../common/vec.h"
#include "../lexer/token.h"
#include "../common/stringpool.h"
#include "../sema/ids/ids.h"  // ExprId — populated by body-store walk

struct Expr;

// Node Id.
//
// NodeId.id is partitioned: the high `NODE_ID_FILE_BITS` bits hold
// the originating file_id, the low `NODE_ID_LOCAL_BITS` bits hold a
// per-parse counter that resets to 1 each parse. This serves two
// invariants at once:
//   1. NodeIds from different inputs never collide, so per-NodeId
//      sema caches (type_of_expr, node_to_decl, resolve_ref_entries,
//      …) stay correct in a multi-module Sema (LSP with N open
//      files).
//   2. NodeIds for the same input are stable across re-parses
//      whenever the AST shape is unchanged, so the invalidation
//      walker can find existing slots and compare fingerprints.
//
// 0 stays the "unset" sentinel — synthesized/placeholder NodeIds
// downstream passes produce won't appear in the cache.
#define NODE_ID_FILE_BITS  12
#define NODE_ID_LOCAL_BITS 20
#define NODE_ID_FILE_SHIFT NODE_ID_LOCAL_BITS
#define NODE_ID_LOCAL_MAX  ((1u << NODE_ID_LOCAL_BITS) - 1u)
#define NODE_ID_FILE_MAX   ((1u << NODE_ID_FILE_BITS) - 1u)

struct NodeId {
    uint32_t id;
};

typedef enum {
    Visibility_private,
    Visibility_public,
} Visibility;

// Identifiers
//
// Pure syntactic shape — name + span. Resolution results live in the
// sema layer's NodeId-keyed `resolved_refs` side-table; query them via
// `query_resolve_ref(s, ident_expr, ns)` rather than reading the AST.
// The AST is immutable post-parse.
struct Identifier {
    StrId string_id;
    struct Span span;
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
    expr_If,         // if/elif/else
    expr_Switch,     // switch expr
    expr_Block,      // { ... }
    expr_Product,    // .{ field = val, ... }
    expr_Bind,       // x := expr, x :: expr
    expr_Ctl,        // ctl(params) ret_type | body
    expr_Handler,    // handler { op :: ... } / handle (target) { ... }
    expr_Mask,       // mask<E>{body}, mask behind<E>{body}
    expr_Field,      // x.name
    expr_Index,      // buf[i]
    expr_Slice,      // buf[start..end] / buf[start..] / buf[..end]
    expr_Lambda,     // |args| body
    expr_Loop,       // Loop cond body
    expr_Struct,     // struct
    expr_Enum,       // enum
    expr_EnumRef,     // enum reference
    decl_Effect,      // effect declaration
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
    expr_FnType,      // `Fn(...) -> R` — type-position-only fn
                      // constructor (capital `Fn`). Lowercase `fn`
                      // remains the value/lambda spelling.
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
    StrId string_id;
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
    unary_Deref,    // x^
    unary_Neg,      // -x
    unary_Not,      // !x
    unary_BitNot,   // ~x
    unary_Const,    // const T
    unary_Optional, // ?T
    unary_DeNil,    // x?
    unary_Inc,      // x++
    unary_Dec,      // x--
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
    StrId name_id; // e.g. "sizeof", "ptrcast", etc.
    Vec* args;
};

// -- If Expressions --

struct IfExpr {
    struct Expr* condition;
    struct Expr* then_branch;
    struct Expr* else_branch; // can be NULL if no else
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
    Visibility visibility;
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
    Visibility visibility;              // per field visibility
    struct Expr* type;                  // Expr* so it can be any type expr
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
};

// -- Effect Row --

struct EffectRowExpr {
    struct Expr* head;              // NULL for <|e>, otherwise the closed effect expression before |
    struct Identifier row;          // effect-row variable after |
};

// -- Mask --
//
// `mask<E>{body}` and `mask behind<E>{body}`. A runtime wrapper that, while
// `body` evaluates, shifts evidence-vector lookup for effect E one handler
// outward (skipping the topmost handler for E). `behind` is a Koka variant
// that tunnels deeper. The handler's `allow_mask` field is consulted at
// runtime to decide whether bypass is permitted.
struct MaskExpr {
    struct Expr* effect;            // type expression inside <...>
    struct Expr* body;               // wrapped expression (typically a Block)
    bool behind;                     // `mask behind<E>{...}` form
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

// -- Slice --
//
// `buf[start..end]` produces a `SliceExpr` with both bounds set;
// `buf[start..]` leaves `end == NULL`; `buf[..end]` leaves
// `start == NULL`. The `..` token only exists inside index brackets
// (mirrors Zig — there's no first-class range value), so all three
// shapes parse from the same suffix path.
struct SliceExpr {
    struct Expr* object;
    struct Expr* start;          // nullable for `[..end]`
    struct Expr* end;            // nullable for `[start..]`
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
typedef enum {
    HandlerSort_Normal,
    HandlerSort_Instance,
} HandlerSort;

typedef enum {
    HandlerScope_NoScope,
    HandlerScope_Scoped,
} HandlerScope;

typedef enum {
    HandlerOverride_None,
    HandlerOverride_Override
} HandlerOverride;

// Tri-state mirroring Koka's `Maybe Bool` for hndlrAllowMask. Most
// handlers leave this Unspecified; explicit Allow/Disallow rules come
// from the `mask<E>` interaction story (sema-level, not parser).
typedef enum {
    HandlerMask_Unspecified,
    HandlerMask_Allow,
    HandlerMask_Disallow,
} HandlerMask;

typedef enum {
    OpVal,
    OpFn,
    OpExcept,
    OpControl,
    OpControlRaw,
    OpControlErr,
} OperationSort;

struct HandlerBranch {
    struct Identifier name;
    Vec* pars;                       // Vec of Param — operation parameters
    struct Expr* expr;               // body expression of the operation
    OperationSort sort;
};

struct HandlerExpr {
    HandlerSort sort;
    HandlerScope scope;
    HandlerOverride override;
    HandlerMask allow_mask;
    struct Expr* effect;             // optional <eff> annotation; NULL if absent.
                                     // Holds an effect-type Expr (Ident, EffectRow,
                                     // etc.) — matches Koka's `Maybe t`.
    struct Expr* initially_clause;
    struct Expr* return_clause;
    struct Expr* finally_clause;
    Vec* branches;                   // Vec of HandlerBranch*
    struct Decl* effect_decl;
    struct Span decl_range;
};

// -- Effect --

typedef enum {
    EFFECT_EXTRA,
    EFFECT_REPLACE
} EffectExtraTag;

typedef struct {
    EffectExtraTag tag;
    Vec* types;                 // Vec of struct Expr* — the umbrella effects
                                // for `effect Foo in Bar` (single-element today,
                                // Vec leaves room for multi-effect rows).
} EffectExtra;

struct OpDecl {
    struct Identifier name;
    Visibility visibility;
    bool is_linear;
    OperationSort sort;
    Vec* params;                // Vec of struct Param
    struct Expr* effect_type;   // NULL if not specified
    struct Expr* result_type;   // The return type of the op
};

struct EffectDecl {
    bool is_linear;
    bool is_named;
    bool is_scoped;
    struct Identifier name;
    EffectExtra extra; // Vec of Extras
    Vec* op_declaration;   // list of operations
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

struct SliceTypeExpr {
    struct Expr* elem;
};

struct ManyPtrType {
    struct Expr* elem;
};

// `Fn(P1, P2, ...) -> R` in type position. Anonymous-typed params; no
// names, no body. This is the type-position-only fn constructor; the
// value-position `fn(name: T, ...) { body }` keeps using LambdaExpr.
//
// Splitting these used to be a parser-side problem: `fn(i32) -> i32`
// could be either "param named i32 with no annotation" or "anonymous
// param of type i32" depending on context, and sema had to disambiguate
// via a primitive-name heuristic. Capital `Fn` keyword is type-only,
// lowercase `fn` is value-only — no ambiguity, no heuristic.
struct FnTypeExpr {
    Vec* param_types;        // Vec<struct Expr*> — type expressions, one
                             //  per param. May be empty for `Fn() -> R`.
    struct Expr* ret_type;   // NULL for void return (parser fills with
                             //  void if not present, but accept NULL
                             //  defensively in sema).
};


// -- the Expr Node ---

struct Expr {
    enum ExprKind kind;
    struct NodeId id;             // assigned monotonically during parse;
                                  // 0 means unset/synthetic. Used as the
                                  // stable handle for query-based passes
                                  // (resolver, sema, codegen) so they can
                                  // key side-tables without holding raw
                                  // arena pointers.
    ExprId expr_id;               // populated by the body-store walk
                                  // (sema/body/body_store.c). EXPR_ID_NONE
                                  // until the owning decl's body store has
                                  // been computed at least once. Replaces
                                  // NodeId as the primary cache key for
                                  // body-level slot tables — stable across
                                  // text-insert-before edits to siblings.
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
        struct StructExpr struct_expr;
        struct FieldExpr field;
        struct IndexExpr index;
        struct LambdaExpr lambda;
        struct SwitchExpr switch_expr;
        struct LoopExpr loop_expr;
        struct EnumExpr enum_expr;
        struct EnumVariant enum_variant_expr;
        struct EnumRefExpr enum_ref_expr;
        struct EffectDecl effect;
        struct EffectRowExpr effect_row;
        struct MaskExpr mask;
        struct { StrId string_id; } asm_expr;
        struct { struct Expr* value; } return_expr;
        struct { struct Expr* value; } defer_expr;
        struct { struct Expr* size; struct Expr* elem; } array_type;
        struct SliceTypeExpr slice_type;
        struct SliceExpr slice;
        struct ManyPtrType many_ptr_type;
        struct FnTypeExpr fn_type;
        struct ArrayLitExpr array_lit;
        struct DestructureBindExpr destructure;
        // break and continue have no payload — just the kind + span
    };
};

// ----- Shared AST helpers -----
//
// `expr_Ident` and `expr_Field` are the two AST kinds whose name we
// often want to extract. Resolution-result lookup lives in sema —
// callers wanting the resolved DefId go through query_resolve_ref /
// query_resolve_path, not the AST.
static inline StrId ast_name_id_of(struct Expr* expr) {
    if (!expr) return STR_ID_NONE;
    if (expr->kind == expr_Ident) return expr->ident.string_id;
    if (expr->kind == expr_Field) return expr->field.field.string_id;
    return STR_ID_NONE;
}

#endif // AST_H
