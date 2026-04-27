#include "./name_resolution.h"
#include <stdlib.h>


struct Resolver resolver_new(Vec* ast, struct StringPool* pool, struct Arena* arena) {
    struct Resolver r = {0};
    r.arena = arena;
    r.pool = pool;
    r.ast = ast;
    r.current = NULL;
    r.root = NULL;
    r.errors = vec_new_in(arena, sizeof(struct ResolveError));
    r.has_errors = false;
    return r;
}

bool resolve(struct Resolver* r) {
    // Create the module scope
    r->root = scope_new(r, SCOPE_MODULE, NULL);
    
    // Pre-populate primitives (i32, u8, bool, void, type, anytype, etc.)
    register_primitives(r);
    
    r->current = r->root;
    
    // Pass 1: collect all top-level names (so forward refs work)
    for (size_t i = 0; i < r->ast->count; i++) {
        struct Expr** decl = (struct Expr**)vec_get(r->ast, i);
        if (decl && *decl) collect_decl(r, *decl);
    }
    
    // Pass 2: resolve every reference
    for (size_t i = 0; i < r->ast->count; i++) {
        struct Expr** decl = (struct Expr**)vec_get(r->ast, i);
        if (decl && *decl) resolve_expr(r, *decl);
    }
    
    return !r->has_errors;
}