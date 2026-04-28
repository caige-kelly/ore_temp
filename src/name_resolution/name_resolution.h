// name_resolution.h

#ifndef NAME_RESOLUTION_H
#define NAME_RESOLUTION_H

#include <stdbool.h>
#include <stdint.h>

#include "../parser/ast.h"
#include "../common/vec.h"
#include "../common/arena.h"
#include "../common/stringpool.h"

// ----- Scopes -----

typedef enum {
    SCOPE_MODULE,     // top-level: a file (anonymous struct)
    SCOPE_FUNCTION,   // fn body
    SCOPE_BLOCK,      // arbitrary { ... }
    SCOPE_STRUCT,     // struct body
    SCOPE_ENUM,       // enum body
    SCOPE_EFFECT,     // effect declaration body
    SCOPE_HANDLER,    // with handler body
    SCOPE_LOOP,       // loop body — break/continue target
    SCOPE_COMPTIME,   // inside a comptime block — effects forbidden
} ScopeKind;

struct Scope {
    ScopeKind kind;
    struct Scope* parent;
    Vec* decls;        // Vec of Decl* — TODO: replace with hashmap (string_id → Decl*) for O(1) lookup once files exceed ~50 decls
    Vec* children;     // Vec of Scope* — for traversal/dump
};

// ----- Declarations -----

typedef enum {
    DECL_PRIMITIVE,   // i32, bool, void, type, ... (node == NULL)
    DECL_USER,        // a user-defined value, type, function — node points to AST
    DECL_IMPORT,      // an @import alias — node points to the @import call, child_scope is the imported module's scope
    DECL_PARAM,       // function/handler parameter
    DECL_FIELD,       // struct/enum/effect member
    DECL_LOOP_LABEL,  // a loop scope handle for break/continue
} DeclKind;

struct Decl {
    DeclKind kind;
    struct Identifier name;
    struct Expr* node;          // AST node that introduced this decl (NULL for primitives)
    struct Scope* owner;        // scope that contains this decl
    struct Scope* child_scope;  // scope INTRODUCED by this decl (modules, structs, enums, effects, fns); NULL otherwise
    struct Module* module;      // for DECL_IMPORT — the imported module
    bool is_comptime;
    bool is_export;             // top-level decl visibility (default true for v1)
    bool has_effects;           // function carries an effect annotation; used by comptime guard
};

// ----- Modules (file = module = anonymous struct) -----

struct Module {
    uint32_t path_id;          // pool offset of the import path / file path
    struct Scope* scope;       // module's top-level scope (kind == SCOPE_MODULE)
    Vec* exports;              // Vec of Decl* — public top-level decls (effectively the "fields" of the anonymous struct)
    Vec* ast;                  // top-level AST (Vec of Expr*)
    bool resolved;             // true once resolve() has finished on this module — used to detect cycles
};

// ----- Errors -----

struct ResolveError {
    struct Span span;
    char msg[256];
};

// ----- Resolver -----

struct Resolver {
    Arena* arena;
    StringPool* pool;
    Vec* ast;                  // top-level AST of the module being resolved
    struct Scope* current;
    struct Scope* root;
    struct Module* current_module;
    Vec* modules;              // Vec of Module* — registry: path → resolved module, dedupe + cycle detection
    Vec* errors;
    bool has_errors;
    int comptime_depth;        // > 0 means we're inside a comptime expression
    Vec* with_imports;         // Vec of Scope* — active `with X` overlays; lookup checks these in addition to parent chain
};

// ----- Public API -----

struct Resolver resolver_new(Vec* ast, StringPool* pool, Arena* arena);
bool resolve(struct Resolver* r);

struct Decl* scope_lookup(struct Scope* s, uint32_t string_id);
void dump_resolution(struct Resolver* r);

// ----- Internals exposed for now (so .c file is small) -----

struct Scope* scope_new(struct Resolver* r, ScopeKind kind, struct Scope* parent);
void register_primitives(struct Resolver* r);
void collect_decl(struct Resolver* r, struct Expr* expr);
void resolve_expr(struct Resolver* r, struct Expr* expr);

#endif
