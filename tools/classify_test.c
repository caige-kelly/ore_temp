// D2.0 gate — semantic-kind classification in the NAMESPACE_ITEMS walk.
//
// Ore is expression-oriented: `Foo :: struct{}` is a SK_BIND_DECL whose
// value is a nameless SK_STRUCT_DECL, and `f :: fn(){}` binds a
// SK_LAMBDA_EXPR. The walk must peek the RHS to record the SEMANTIC kind
// so def_identity classifies the DefId into the right per-kind table —
// otherwise everything is KIND_CONSTANT and the type layer can't route.
//   1. struct bind  → KIND_STRUCT
//   2. fn bind      → KIND_FUNCTION
//   3. plain const  → KIND_CONSTANT
//   4. struct→enum retype (same name) → same DefId (name-only AstId),
//      re-classified KIND_ENUM.
// KEEP_ZONE, ASan.

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
static DefKind kind_of(struct db *s, NamespaceId ns, const char *nm) {
    StrId name = intern(s, nm);
    DefId d = db_query_def_identity(s, ns, astid_of(s, ns, name));
    assert(d.idx != 0 && "decl minted a DefId");
    return db_def_kind(s, d);
}

int main(void) {
    struct db s;
    db_init(&s);
    FileId fid = open_file(&s, "/c.ore",
        "Point :: struct { x: i32, y: i32 }\n"
        "addone :: fn() {}\n"
        "K :: 1\n");
    NamespaceId ns = db_get_file_namespace(&s, fid);

    db_request_begin(&s, db_current_revision(&s));
    assert(kind_of(&s, ns, "Point")  == KIND_STRUCT   && "struct bind → KIND_STRUCT");
    assert(kind_of(&s, ns, "addone") == KIND_FUNCTION && "fn bind → KIND_FUNCTION");
    assert(kind_of(&s, ns, "K")      == KIND_CONSTANT && "plain bind → KIND_CONSTANT");
    StrId point = intern(&s, "Point");
    AstId struct_id = astid_of(&s, ns, point);
    DefId struct_def = db_query_def_identity(&s, ns, struct_id);
    db_request_end(&s);

    // Retype Point struct→enum: same name -> same AstId -> same DefId
    // (identity preserved), but re-classified as KIND_ENUM.
    SourceId sid = db_get_file_source(&s, fid);
    const char *e2 =
        "Point :: enum { A, B }\n"
        "addone :: fn() {}\n"
        "K :: 1\n";
    assert(db_set_source_text(&s, sid, e2, strlen(e2)));

    db_request_begin(&s, db_current_revision(&s));
    AstId enum_id = astid_of(&s, ns, point);
    assert(enum_id.idx == struct_id.idx && "struct→enum keeps the AstId");
    DefId enum_def = db_query_def_identity(&s, ns, enum_id);
    assert(enum_def.idx == struct_def.idx && "retype → same DefId (identity preserved)");
    assert(db_def_kind(&s, enum_def) == KIND_ENUM && "retyped bind → KIND_ENUM");
    db_request_end(&s);

    // Visibility/meta: `pub` is a modifier token after the bind op → VIS_PUBLIC
    // on NamespaceItem.meta (computed by decl_classify in the membership walk).
    // A plain bind is VIS_PRIVATE. Visibility lives ONLY on the membership index
    // now — defs.meta was removed in D2.5 as a dead write-only column; the check
    // driver's unused pass reads NamespaceItem.meta directly.
    const char *e3 =
        "Exported :: pub struct { x: i32 }\n"
        "secret :: struct { y: i32 }\n";
    assert(db_set_source_text(&s, sid, e3, strlen(e3)));
    db_request_begin(&s, db_current_revision(&s));
    StrId exp = intern(&s, "Exported"), sec = intern(&s, "secret");
    FileArray items3 = db_query_namespace_items(&s, ns);
    const NamespaceItem *a3 = (const NamespaceItem *)items3.data;
    DefMeta exp_meta = 0, sec_meta = 0;
    for (uint32_t i = 0; i < items3.count; i++) {
        if (a3[i].name.idx == exp.idx) exp_meta = a3[i].meta;
        if (a3[i].name.idx == sec.idx) sec_meta = a3[i].meta;
    }
    assert((exp_meta & META_VIS_MASK) == VIS_PUBLIC  && "pub decl → VIS_PUBLIC");
    assert((sec_meta & META_VIS_MASK) == VIS_PRIVATE && "plain decl → VIS_PRIVATE");
    db_request_end(&s);

    db_free(&s);
    printf("PASS classify: struct/fn/const kinds + struct→enum retype (new DefId "
           "KIND_ENUM) + pub→VIS_PUBLIC meta\n");
    return 0;
}
