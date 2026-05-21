#ifndef ORE_PARSER_AST_H
#define ORE_PARSER_AST_H

#include <stdint.h>
#include <stdbool.h>

#include "../db/ids/ids.h"           // AstNodeId, StrId
#include "../db/storage/arena.h"
#include "../db/storage/vec.h"

/* 
    The Durable AST Store.

    This represents the "what" of the syntax tree, bitwise-stable across
    whitespace/trivia changes. Source positioning, parent mapping, and 
    type mapping are volatile side-tables stored in `ModuleInfo`, NOT here.

    Layout: Struct-of-Arrays (SoA)
*/

// 1 byte
typedef enum : uint8_t {
    AST_ERROR = 0,

    // Declarations
    AST_DECL_MODULE,
    AST_DECL_STRUCT,
    AST_DECL_ENUM,
    AST_DECL_UNION,
    AST_DECL_EFFECT,
    AST_DECL_CONST,
    AST_DECL_VAR,
    AST_DECL_DESTRUCTURE,             // `.{a,b} := expr` — pattern bind

    // Declarations — structural sub-nodes (one tag per shape, no extras polymorphism).
    AST_DECL_PARAM,                   // fn/lambda param: [name, type, is_comptime]
    AST_DECL_FIELD,                   // struct/union field: [name (0=anon), type, vis, fpos]
    AST_DECL_VARIANT,                 // enum variant:        [name, value (0=auto)]

    // (Modifiers are NOT AST nodes — they bitpack into the DefMeta byte
    //  carried on each decl's extras and on TopLevelEntry.)

    // Statements
    AST_STMT_BLOCK,
    AST_STMT_RETURN,
    AST_STMT_IF,
    AST_STMT_LOOP,
    AST_STMT_SWITCH,
    AST_STMT_SWITCH_ARM,              // switch arm: [pat_count, pat0..N, body]
    AST_STMT_BREAK,
    AST_STMT_CONTINUE,
    AST_STMT_DEFER,

    // (Built-in primitive types — bool, i32, u8, void, noreturn, type,
    //  anytype, comptime_int/float, error, … — all flow through
    //  AST_EXPR_PATH. The lexer treats them as plain identifiers and
    //  sema's primitives table resolves them via s->names equality.
    //  Zig-style universal-identifier model.)

    // Expressions - Literals
    AST_EXPR_LIT_INT,
    AST_EXPR_LIT_FLOAT,
    AST_EXPR_LIT_STRING,
    AST_EXPR_LIT_BYTE,
    AST_EXPR_LIT_BOOL,
    AST_EXPR_LIT_NIL,
    AST_EXPR_ASM,
    AST_EXPR_WILDCARD,                // `_`

    // Expressions - Binary
    AST_EXPR_BIN_ADD,
    AST_EXPR_BIN_SUB,
    AST_EXPR_BIN_MUL,
    AST_EXPR_BIN_DIV,
    AST_EXPR_BIN_MOD,
    AST_EXPR_BIN_POW,                 // **
    AST_EXPR_BIN_EQ,
    AST_EXPR_BIN_NEQ,
    AST_EXPR_BIN_LT,
    AST_EXPR_BIN_LE,
    AST_EXPR_BIN_GT,
    AST_EXPR_BIN_GE,
    AST_EXPR_BIN_AND,
    AST_EXPR_BIN_OR,
    AST_EXPR_BIN_ORELSE,              // nullable coalesce
    AST_EXPR_BIN_CATCH,               // error coalesce
    AST_EXPR_BIN_BIT_AND,
    AST_EXPR_BIN_BIT_OR,
    AST_EXPR_BIN_BIT_XOR,
    AST_EXPR_BIN_SHL,
    AST_EXPR_BIN_SHR,

    // Expressions - Assignments
    AST_EXPR_ASSIGN,
    AST_EXPR_ASSIGN_ADD,
    AST_EXPR_ASSIGN_SUB,
    AST_EXPR_ASSIGN_MUL,
    AST_EXPR_ASSIGN_DIV,
    AST_EXPR_ASSIGN_MOD,
    AST_EXPR_ASSIGN_BIT_AND,
    AST_EXPR_ASSIGN_BIT_OR,
    AST_EXPR_ASSIGN_BIT_XOR,

    // Expressions - Prefix unary
    AST_EXPR_UNARY_NEG,
    AST_EXPR_UNARY_NOT,
    AST_EXPR_UNARY_BIT_NOT,
    AST_EXPR_UNARY_REF,               // &x
    AST_EXPR_UNARY_DEREF,             // *x or postfix x^
    // (Type-position prefix-ops `^T` / `?T` / `const T` are NOT in
    //  the value-unary family — they're AST_TYPE_PTR/OPTIONAL/CONST
    //  in the Types section below. Zig-style: type constructors get
    //  their own AST tags, distinct from value-position unaries.)

    // Expressions - Postfix unary
    AST_EXPR_UNARY_INC,               // x++
    AST_EXPR_UNARY_DENIL,             // x? unwrap-optional
    AST_EXPR_UNARY_DEERR,             // x! unwrap-error

    // Expressions - Other
    AST_EXPR_CALL,
    AST_EXPR_INDEX,
    AST_EXPR_SLICE,                   // x[a..b] etc.
    AST_EXPR_FIELD,
    AST_EXPR_PATH,
    AST_EXPR_LAMBDA,                  // fn(...) body
    AST_EXPR_HANDLE,                  // handle(target) { ... }
    AST_EXPR_HANDLER,                 // handler { ... } as a value
    AST_EXPR_MASK,                    // mask<E>{body} / mask behind<E>{body}
    AST_EXPR_PRODUCT,                 // .{ ... } and T{ ... }
    AST_INIT_FIELD,                   // .x = value (one product/init entry): [name (0=positional), value]
    AST_EXPR_ENUM_REF,                // .Variant
    AST_EXPR_BUILTIN,                 // @name(args)
    AST_EXPR_EFFECT_ROW,              // <H | e>

    // Types
    // (AST_TYPE_PATH removed — Zig precedent: one universal identifier
    //  tag (AST_EXPR_PATH) for all name references; sema disambiguates
    //  type-vs-value via primitives table + parent context.)
    AST_TYPE_PTR,
    AST_TYPE_SLICE,
    AST_TYPE_ARRAY,
    AST_TYPE_MANYPTR,                 // [^]T
    AST_TYPE_FN,
    AST_TYPE_OPTIONAL,
    AST_TYPE_CONST,                   // const T

} AstNodeKind;

// 8-byte payload. FIRST MEMBER MUST BE 8 BYTES (int_val): the durable
// AST fingerprint hashes this union's full 8-byte representation for
// every node, and `AstNodeData d = {0};` (the construction idiom used
// everywhere) per C only initializes the FIRST union member. With a
// 4-byte first member, nodes that set a <=4-byte member left bytes 4..7
// indeterminate (arena/stack garbage) → the fingerprint drifted across
// builds (deterministic within a build, but not a pure function of AST
// content). An 8-byte first member makes `= {0}` zero all 8 bytes, so
// every stored AstNodeData is canonical. Do not reorder.
typedef union {
    uint64_t int_val;        // first: forces `= {0}` to zero all 8 bytes
    double float_val;

    AstExtraDataIdx extra_idx;

    struct {
        AstNodeId lhs;
        AstNodeId rhs;
    } bin;

    AstNodeId single_child;

    StrId string_id;
    bool bool_val;
} AstNodeData;

// Durable AST Store (owned by ModuleInfo.ast)
typedef struct ASTStore {
    Vec kinds;       // Vec<AstNodeKind>
    Vec main_tokens; // Vec<uint32_t> (index into module's tokens)
    Vec data;        // Vec<AstNodeData>
    Vec extra;       // Vec<uint32_t>
    
    Arena *arena;
} ASTStore;

void ast_store_init(ASTStore *ast, Arena *arena, size_t max_nodes);
AstNodeId ast_push_node(ASTStore *ast, AstNodeKind kind, uint32_t main_token, AstNodeData data);
AstExtraDataIdx ast_push_extra(ASTStore *ast, const uint32_t *items, uint32_t count);

// Visit every AST-node child of `id`, invoking `fn(child, ctx)` once
// per child. Knows the per-kind extras layout — single source of truth
// for "what node ids does each kind reference," shared by populate_parents
// and any future structural walker (sema parent-walks, dependency tracking,
// etc.). Skips non-node slots (StrIds, raw token indices, flag/meta u32s).
// `fn` is never called with the NONE sentinel.
typedef void (*AstChildFn)(AstNodeId child, void *ctx);
void ast_visit_children(const ASTStore *ast, AstNodeId id,
                        AstChildFn fn, void *ctx);

// Release the malloc-backed Vecs (kinds/main_tokens/data/extra). Does
// NOT free the ASTStore struct itself — that lives in the per-module
// arena and is reclaimed by arena_reset/arena_free.
void ast_store_free(ASTStore *ast);

#endif // ORE_PARSER_AST_H