#include "./name_resolution.h"
#include <stdlib.h>

Scope* scope_new(Arena* arena, ScopeKind kind, Scope* parent) {
    Arena* a = malloc(sizeof(Arena));
    arena_init(a, 1 * sizeof(struct Expr) * 2);
    
}


void resolve_module(Resolver* r, Vec* top_level_decls) {
    Scope* mod_scope = scope_new(r->arena, SCOPE_MODULE, NULL)

    register_primitives(r, mod_scope);

}