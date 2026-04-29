// name_resolution.h

#ifndef NAME_RESOLUTION_H
#define NAME_RESOLUTION_H

#include <stdbool.h>
#include <stdint.h>

#include "../parser/ast.h"
#include "../common/vec.h"
#include "../common/arena.h"
#include "../common/stringpool.h"
#include "../diag/diag.h"
#include "../diag/sourcemap.h"
#include "../sema/query.h"

struct Compiler;

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
    DECL_SCOPE_PARAM, // comptime-only effect scope token, e.g. <s>
    DECL_EFFECT_ROW,  // effect-row variable, e.g. <|e> / <Effect | e>
    DECL_LOOP_LABEL,  // a loop scope handle for break/continue
} DeclKind;

typedef enum {
    SEM_UNKNOWN,
    SEM_VALUE,
    SEM_TYPE,
    SEM_EFFECT,
    SEM_MODULE,
    SEM_SCOPE_TOKEN,
    SEM_EFFECT_ROW,
} SemanticKind;

struct Decl {
    DeclKind kind;
    SemanticKind semantic_kind;
    struct Identifier name;
    struct Expr* node;          // AST node that introduced this decl (NULL for primitives)
    struct Scope* owner;        // scope that contains this decl
    struct Scope* child_scope;  // scope INTRODUCED by this decl (modules, structs, enums, effects, fns); NULL otherwise
    struct Module* module;      // for DECL_IMPORT — the imported module
    struct QuerySlot type_query;
    struct Type* type;          // canonical type for this binding; NULL until sema fills it
    struct EffectSig* effect_sig;
    bool is_comptime;
    bool is_export;             // top-level decl visibility (default true for v1)
    bool has_effects;           // function carries an effect annotation; used by comptime guard
    // Fresh skolem-ish id for DECL_SCOPE_PARAM (0 otherwise). This is
    // the region/color handle future borrow-lite escape analysis can
    // imprint on references produced by scoped handlers/resources.
    uint32_t scope_token_id;
};

// ----- Modules (file = module = anonymous struct) -----

struct Module {
    uint32_t path_id;          // pool offset of the import path / file path
    struct Scope* scope;       // module's top-level scope (kind == SCOPE_MODULE)
    Vec* exports;              // Vec of Decl* — public top-level decls (effectively the "fields" of the anonymous struct)
    Vec* ast;                  // top-level AST (Vec of Expr*)
    bool resolving;            // true while resolve_module() is on the import stack
    bool resolved;             // true once resolve() has finished on this module — used to detect cycles
};

// ----- Resolver -----

struct Resolver {
    struct Compiler* compiler;
    Arena* arena;
    StringPool* pool;
    Vec* ast;                  // top-level AST of the module being resolved
    struct Scope* current;
    struct Scope* root;
    struct Module* current_module;
    uint32_t root_path_id;     // canonical root file path, interned in the string pool
    struct SourceMap* source_map;
    struct DiagBag* diags;
    bool has_errors;
    int comptime_depth;        // > 0 means we're inside a comptime expression
    int effect_annotation_depth; // > 0 means scope/effect-row tokens may be referenced
    int loop_body_depth;       // > 0 means break/continue may target an enclosing loop body
    uint32_t next_scope_token_id;
    Vec* with_imports;         // Vec of Scope* — active `with X` overlays; lookup checks these in addition to parent chain
};

// ----- Public API -----

struct Resolver resolver_new(struct Compiler* compiler, Vec* ast);
bool resolve(struct Resolver* r);

struct Decl* scope_lookup(struct Scope* s, uint32_t string_id);
void dump_resolution(struct Resolver* r);

// ----- Internals exposed for now (so .c file is small) -----

struct Scope* scope_new(struct Resolver* r, ScopeKind kind, struct Scope* parent);
void register_primitives(struct Resolver* r);
void collect_decl(struct Resolver* r, struct Expr* expr);
void resolve_expr(struct Resolver* r, struct Expr* expr);

#endif
