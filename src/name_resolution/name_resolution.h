// name_resolution.h

#ifndef NAME_RESOLUTION_H
#define NAME_RESOLUTION_H

#include <stdbool.h>
#include <stdint.h>


#include "./src/parser/ast.h"

typedef enum {
    SCOPE_MODULE,
    SCOPE_FUNCTION,
    SCOPE_BLOCK,
    SCOPE_STRUCT,
    SCOPE_ENUM,
    SCOPE_EFFECT,
    SCOPE_HANDLER,
} ScopeKind;

struct Decl {
    struct Identifier name;
    struct Expr* node;
    struct Scope* scope;
    bool is_comptime;
};

struct Scope {
    ScopeKind kind;
    struct Scope* parent;
    Vec* decls;                   // Vec of Decl* (or use a hashmap if performance matters)
    Vec* children;
};

struct ResolveError {
    struct Span span;
    char msg[256];
};

struct Resolver {
    struct Arena* arena;
    struct StringPool* pool;
    Vec* ast;
    struct Scope* current;
    struct Scope* root;
    Vec* errors;
    bool has_errors;
};

struct Resolver resolver_new(Vec* ast, struct StringPool* pool, struct Arena* arena);
bool resolve(struct Resolver* r);

// Maybe expose for tests / dumps
struct Decl* scope_lookup(struct Scope* s, uint32_t string_id);
void dump_resolution(struct Resolver* r);

#endif