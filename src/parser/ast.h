#ifndef ORE_PARSER_AST_H
#define ORE_PARSER_AST_H

#include <stdint.h>
#include <stdbool.h>
#include "../lexer/token.h"
#include "../common/arena.h"
#include "../common/vec.h"
#include "../common/stringpool.h"

/* Core Types */

// Strongly-typed handle to an AST node. 0 is always invalid.
typedef struct { uint32_t raw; } AstNodeId;

// Strongly-typed handle to an index in the extra_data bucket.
typedef struct { uint32_t raw; } AstExtraDataIdx;

#define AST_NODE_NONE ((AstNodeId){0})

static inline bool ast_id_valid(AstNodeId id) { return id.raw != 0; }

/* Granular Kinds (Operators are kinds, no data) */
typedef enum {
    AST_NODE_NONE = 0,

    // Literals
    AST_EXPR_LIT_INT,
    AST_EXPR_LIT_STRING,
    AST_EXPR_LIT_BOOL,

    // Binary Expressions
    AST_EXPR_BIN_ADD,
    AST_EXPR_BIN_SUB,
    AST_EXPR_BIN_MUL,
    AST_EXPR_BIN_DIV,
    AST_EXPR_BIN_ASSIGN,
    AST_EXPR_BIN_EQ,
    AST_EXPR_BIN_NEQ,
    AST_EXPR_BIN_GT,
    AST_EXPR_BIN_LT,
    AST_EXPR_BIN_GTEQ,
    AST_EXPR_BIN_LTEQ,

    // Unary Expressions
    AST_EXPR_UNARY_NEGATE,
    AST_EXPR_UNARY_NOT,
    
    // Control flow & grouping
    AST_EXPR_GROUPING,
    AST_STMT_EXPR,
    AST_STMT_RETURN,

    // Large nodes (uses extra_data)
    AST_EXPR_CALL,   // Needs callee, args_list
    AST_DECL_FUNC,   // Needs name, type, params, body
    AST_DECL_STRUCT, // Needs name, fields
} AstNodeKind;

/* The 8-byte Payload Union */
typedef union {
    // Large nodes index into extra_data array
    AstExtraDataIdx extra_data_idx;

    // all binary ops
    struct {AstNodeId lhs; AstNodeId rhs;} pair;

    // Unary Ops
    AstNodeId single_child;

    // Literals (Semantic values only, no Spans or Tokens)
    uint64_t int_val;
    StrId string_id;
    bool  bool_val;
    double float_val;

} AstNodeData;

/* the ASTStore database */
typedef struct {
    // Core node data (stored by value to avoid pointer derefs)
    Vec node_kinds; // Vec<AstNodeKind>
    Vec node_data;  // Vec<AstNodeData>

    // Bucket for nodes >8 bytes
    Vec extra_data; // Vec<uint32_t>

    // Side tables (Pointers to be around independent)
    Vec span_map; // Vec<Span>
    Vec parent_map; // Vec<AstNodeId>
    Vec trivia_map; // Vec<Token>
    Vec type_map;

    // Memory backing
    Arena* arena;
} ASTStore;

/* API */
void ast_store_init(ASTStore* store, Arena* arena);
AstNodeId ast_push_node(ASTStore* ast, AstNodeKind kind, AstNodeData data, struct Span span);
void build_parent_map(ASTStore* ast);

#endif // ORE_PARSER_AST_H