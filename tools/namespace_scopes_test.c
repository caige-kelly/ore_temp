// D1 gate — namespace_scopes: builds the internal name scope.
//   - internal scope created (exported deferred → NONE);
//   - parented to the primitives scope;
//   - holds a {name → DefId} binding per top-level decl, where the DefId
//     matches def_identity's.
// KEEP_ZONE, ASan.

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/query/engine.h"
#include "../src/db/workspace/workspace.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

extern NamespaceScopes db_query_namespace_scopes(db_query_ctx *ctx, NamespaceId nsid);
extern FileArray       db_query_namespace_items(db_query_ctx *ctx, NamespaceId nsid);
extern DefId           db_query_def_identity(db_query_ctx *ctx, NamespaceId nsid, AstId id);

static FileId open_file(struct db *s, const char *path, const char *text) {
    SourceId src = workspace_did_open(s, path, strlen(path), text, strlen(text));
    return db_lookup_file_by_source(s, src);
}
static StrId intern(struct db *s, const char *str) {
    return pool_intern(&s->strings, str, strlen(str));
}
static AstId astid_of(struct db *s, NamespaceId ns, StrId name) {
    FileArray items = db_query_namespace_items(s, ns);
    const NamespaceItem *a = (const NamespaceItem *)items.data;
    for (uint32_t i = 0; i < items.count; i++)
        if (a[i].name.idx == name.idx) return a[i].id;
    return AST_ID_NONE;
}
static DefId scope_lookup(struct db *s, ScopeId scope, StrId name) {
    uint32_t lo  = *(uint32_t *)vec_get(&s->scopes.decl_lo, scope.idx);
    uint32_t len = *(uint32_t *)vec_get(&s->scopes.decl_len, scope.idx);
    const DeclEntry *pool = (const DeclEntry *)s->scopes.decl_pool.data;
    for (uint32_t i = 0; i < len; i++)
        if (pool[lo + i].name.idx == name.idx) return pool[lo + i].def;
    return DEF_ID_NONE;
}

int main(void) {
    struct db s;
    db_init(&s);
    FileId fid = open_file(&s, "/ns.ore", "foo :: 1\nbar :: 2\n");
    NamespaceId ns = db_get_file_namespace(&s, fid);
    StrId foo = intern(&s, "foo"), bar = intern(&s, "bar");

    db_request_begin(&s, db_current_revision(&s));
    NamespaceScopes sc = db_query_namespace_scopes(&s, ns);
    assert(sc.internal.idx != 0 && "internal scope created");
    assert(sc.exported.idx == 0 && "exported scope deferred (NONE)");

    ScopeId parent = *(ScopeId *)vec_get(&s.scopes.parents, sc.internal.idx);
    assert(parent.idx == s.primitives_scope.idx &&
           "internal scope parented to the primitives scope");

    DefId sfoo = scope_lookup(&s, sc.internal, foo);
    DefId sbar = scope_lookup(&s, sc.internal, bar);
    DefId dfoo = db_query_def_identity(&s, ns, astid_of(&s, ns, foo));
    assert(sfoo.idx != 0 && sbar.idx != 0 && "both names bound in the scope");
    assert(sfoo.idx == dfoo.idx && "scope binding == def_identity's DefId");
    db_request_end(&s);

    db_free(&s);
    printf("PASS namespace_scopes: internal name->DefId bindings, "
           "parent=primitives, exported=NONE\n");
    return 0;
}
