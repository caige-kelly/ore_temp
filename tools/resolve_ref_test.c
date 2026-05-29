// D1 gate — resolve_ref: scope-chain name resolution.
//   - a local top-level name resolves to its DefId (== def_identity's);
//   - an unbound name falls through to the parent (primitives) scope and
//     resolves a primitive (`u32`);
//   - an unknown name → DEF_ID_NONE.
// KEEP_ZONE, ASan.

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/query/engine.h"
#include "../src/db/workspace/workspace.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

extern NamespaceScopes db_query_namespace_scopes(db_query_ctx *ctx, NamespaceId nsid);
extern DefId           db_query_resolve_ref(db_query_ctx *ctx, ScopeId scope, StrId name);
extern DefId           db_query_def_identity(db_query_ctx *ctx, NamespaceId nsid, AstId id);
extern FileArray       db_query_namespace_items(db_query_ctx *ctx, NamespaceId nsid);

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

int main(void) {
    struct db s;
    db_init(&s);
    FileId fid = open_file(&s, "/r.ore", "foo :: 1\nbar :: 2\n");
    NamespaceId ns = db_get_file_namespace(&s, fid);

    db_request_begin(&s, db_current_revision(&s));
    ScopeId internal = db_query_namespace_scopes(&s, ns).internal;
    StrId foo = intern(&s, "foo");
    DefId rfoo  = db_query_resolve_ref(&s, internal, foo);
    DefId dfoo  = db_query_def_identity(&s, ns, astid_of(&s, ns, foo));
    DefId ru32  = db_query_resolve_ref(&s, internal, intern(&s, "u32"));
    DefId rmiss = db_query_resolve_ref(&s, internal, intern(&s, "nonexistent"));
    db_request_end(&s);

    assert(rfoo.idx != 0 && rfoo.idx == dfoo.idx &&
           "local name resolves to its DefId");
    assert(ru32.idx != 0 && "unbound name falls through to the primitives scope");
    assert(rmiss.idx == 0 && "unknown name → DEF_ID_NONE");

    db_free(&s);
    printf("PASS resolve_ref: local resolve, primitive fall-through, miss → NONE\n");
    return 0;
}
