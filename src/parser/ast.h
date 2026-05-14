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
    AST_DECL_IMPORT,
    AST_DECL_FN,
    AST_DECL_STRUCT,
    AST_DECL_ENUM,
    AST_DECL_UNION,
    AST_DECL_EFFECT,
    AST_DECL_CONST,
    AST_DECL_VAL,
    
    // Statements
    AST_STMT_BLOCK,
    AST_STMT_EXPR,
    AST_STMT_RETURN,
    AST_STMT_IF,
    AST_STMT_LOOP,
    AST_STMT_BREAK,
    AST_STMT_CONTINUE,
    AST_STMT_DEFER,
    
    // Expressions - Literals
    AST_EXPR_LIT_INT,
    AST_EXPR_LIT_FLOAT,
    AST_EXPR_LIT_STRING,
    AST_EXPR_LIT_BOOL,
    AST_EXPR_LIT_NIL,
    
    // Expressions - Binary
    AST_EXPR_BIN_ADD,
    AST_EXPR_BIN_SUB,
    AST_EXPR_BIN_MUL,
    AST_EXPR_BIN_DIV,
    AST_EXPR_BIN_MOD,
    AST_EXPR_BIN_EQ,
    AST_EXPR_BIN_NEQ,
    AST_EXPR_BIN_LT,
    AST_EXPR_BIN_LE,
    AST_EXPR_BIN_GT,
    AST_EXPR_BIN_GE,
    AST_EXPR_BIN_AND,
    AST_EXPR_BIN_OR,
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
    
    // Expressions - Unary/Other
    AST_EXPR_UNARY_NEG,
    AST_EXPR_UNARY_NOT,
    AST_EXPR_UNARY_BIT_NOT,
    AST_EXPR_UNARY_REF,
    AST_EXPR_UNARY_DEREF,
    
    AST_EXPR_CALL,
    AST_EXPR_INDEX,
    AST_EXPR_FIELD,
    AST_EXPR_PATH,
    AST_EXPR_GROUP,
    
    // Types
    AST_TYPE_PATH,
    AST_TYPE_PTR,
    AST_TYPE_SLICE,
    AST_TYPE_ARRAY,
    AST_TYPE_FN,
    AST_TYPE_OPTIONAL,
    
} AstNodeKind;

// 8 bytes payload
typedef union {
    AstExtraDataIdx extra_idx;
    
    struct {
        AstNodeId lhs;
        AstNodeId rhs;
    } bin;
    
    AstNodeId single_child;
    
    StrId string_id;
    uint64_t int_val;
    double float_val;
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

#endif // ORE_PARSER_AST_H