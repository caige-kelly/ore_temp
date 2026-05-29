// D1 gate — def_identity: canonical, reparse-STABLE DefId (interning).
//   - same decl → same DefId across a sibling edit (the AstId-keyed
//     interning guarantee — no churn);
//   - rename → a NEW DefId (identity changed; old one orphaned).
// KEEP_ZONE, ASan (mint must be leak-free + idempotent).

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/query/engine.h"
#include "../src/db/workspace/workspace.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

extern FileArray db_query_namespace_items(db_query_ctx *ctx, NamespaceId nsid);
extern DefId     db_query_def_identity(db_query_ctx *ctx, NamespaceId nsid, AstId id);

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
    FileId fid = open_file(&s, "/d.ore", "foo :: 1\nbar :: 2\n");
    NamespaceId ns = db_get_file_namespace(&s, fid);
    StrId foo = intern(&s, "foo");

    db_request_begin(&s, db_current_revision(&s));
    DefId def_foo1 = db_query_def_identity(&s, ns, astid_of(&s, ns, foo));
    db_request_end(&s);
    assert(def_foo1.idx != 0 && "foo minted a DefId");

    // Edit a SIBLING (bar) → foo's DefId must be unchanged.
    SourceId sid = db_get_file_source(&s, fid);
    const char *e2 = "foo :: 1\nbar :: 99999\n";
    assert(db_set_source_text(&s, sid, e2, strlen(e2)));
    db_request_begin(&s, db_current_revision(&s));
    DefId def_foo2 = db_query_def_identity(&s, ns, astid_of(&s, ns, foo));
    db_request_end(&s);
    assert(def_foo2.idx == def_foo1.idx && "DefId STABLE across a sibling edit");

    // Rename foo → qux → a different identity → a new DefId.
    const char *e3 = "qux :: 1\nbar :: 99999\n";
    assert(db_set_source_text(&s, sid, e3, strlen(e3)));
    StrId qux = intern(&s, "qux");
    db_request_begin(&s, db_current_revision(&s));
    DefId def_qux = db_query_def_identity(&s, ns, astid_of(&s, ns, qux));
    db_request_end(&s);
    assert(def_qux.idx != 0 && def_qux.idx != def_foo1.idx &&
           "rename → new DefId (old orphaned)");

    db_free(&s);
    printf("PASS def_identity: stable DefId across sibling edit; rename → new DefId\n");
    return 0;
}
