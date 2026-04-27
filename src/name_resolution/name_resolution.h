#ifndef NAME_RESOLUTION_H
#define NAME_RESOLUTION_H

#include <stdbool.h>
#include "./src/common/vec.h"
#include "./src/parser/ast.h"
#include "./src/common/arena.h"
#include "./src/common/stringpool.h"


typedef struct Hashmap {} HashMap;

typedef enum {
    SCOPE_MODULE,
    SCOPE_FUNCTION,
    SCOPE_BLOCK,
    SCOPE_STRUCT,
    SCOPE_ENUM,
    SCOPE_EFFECT,
    SCOPE_HANDLER,
} ScopeKind;

typedef struct Decl {
    struct Identifier name;
    struct Expr* node;             // the AST node that declared it
    struct Scope* scope;           // scope where declared
    
    // Metadata
    bool is_comptime;              // const binding (::)
    bool is_type;                  // detected as a type at comptime
    
    // Filled in by comptime evaluator later (NULL during name res)
    struct Value* comptime_value;
} Decl;

typedef struct Scope {
    ScopeKind kind;
    struct Scope* parent;
    HashMap* decls;                // string_id -> Decl*
    Vec* children;                 // child scopes
    
    // For SCOPE_MODULE: the implicit struct this module represents
    struct Expr* module_struct;
    
    // For SCOPE_FUNCTION: the function's params are in this scope
    struct Expr* function;
} Scope;

typedef struct Resolver {
    Arena* arena;
    Scope* current;
    Vec* errors;
    StringPool* pool;
} Resolver;

#endif //NAME_RESOLUTION_H