// C.2c gate — NAMESPACE_ITEMS: the per-namespace items index + AstId.
//
//   1. ENUMERATION — query NAMESPACE_ITEMS(ns) directly and get EVERY
//      top-level name, with no per-name query. This is the capability the
//      per-name top_level_entry structurally cannot provide.
//   2. INDEX FIREWALL — a pure trivia shift (prepend a comment) reparses
//      file_ast and moves every decl's byte range, but the index slot fp
//      is UNCHANGED (it folds AstId + structural hash, never ranges) while
//      top_level_entry's returned node_ptr is CURRENT and its AstId stable.
//   3. CONTENT edit — changing a decl's value changes the index fp.
//   4. RENAME — renaming a decl changes that item's AstId (kind+name).
// KEEP_ZONE, ASan.

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/query/engine.h"
#include "../src/db/workspace/workspace.h"
#include "../src/syntax/syntax.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

extern FileArray     db_query_namespace_items(db_query_ctx *ctx, NamespaceId nsid);
extern TopLevelEntry db_query_top_level_entry(db_query_ctx *ctx,
                                              NamespaceId nsid, StrId name);

static FileId open_file(struct db *s, const char *path, const char *text) {
    SourceId src = workspace_did_open(s, path, strlen(path), text, strlen(text));
    return db_lookup_file_by_source(s, src);
}

static StrId intern(struct db *s, const char *str) {
    return pool_intern(&s->strings, str, strlen(str));
}

// True iff the index contains an item named `name`.
static bool items_have(FileArray items, StrId name) {
    const NamespaceItem *arr = (const NamespaceItem *)items.data;
    for (uint32_t i = 0; i < items.count; i++)
        if (arr[i].name.idx == name.idx) return true;
    return false;
}

static AstId item_ast_id(FileArray items, StrId name) {
    const NamespaceItem *arr = (const NamespaceItem *)items.data;
    for (uint32_t i = 0; i < items.count; i++)
        if (arr[i].name.idx == name.idx) return arr[i].id;
    return AST_ID_NONE;
}

int main(void) {
    struct db s;
    db_init(&s);
    FileId fid = open_file(&s, "/items.ore", "foo :: 1\nbar :: 2\nbaz :: 3\n");
    NamespaceId ns = db_get_file_namespace(&s, fid);
    StrId foo = intern(&s, "foo"), bar = intern(&s, "bar"), baz = intern(&s, "baz");

    // (1) Enumeration — the whole set, no per-name query.
    db_request_begin(&s, db_current_revision(&s));
    FileArray items = db_query_namespace_items(&s, ns);
    Fingerprint ifp1 = db_slot_fingerprint(&s, QUERY_NAMESPACE_ITEMS, (uint64_t)ns.idx);
    assert(items.count == 3 && "enumerated all three top-level items");
    assert(items_have(items, foo) && items_have(items, bar) && items_have(items, baz) &&
           "every name present in the index");
    AstId bar_id1 = item_ast_id(items, bar);
    // top_level_entry reads the index; capture bar's ptr + fp.
    TopLevelEntry e_bar1 = db_query_top_level_entry(&s, ns, bar);
    Fingerprint  tfp1 = db_slot_fingerprint(&s, QUERY_TOP_LEVEL_ENTRY,
                                            ((uint64_t)ns.idx << 32) | (uint32_t)bar.idx);
    uint32_t bar_start1 = e_bar1.node_ptr.range.start;
    db_request_end(&s);
    assert(ifp1 != FINGERPRINT_NONE && "index fp non-empty");
    assert(e_bar1.id.idx == bar_id1.idx && "top_level_entry carries the index AstId");

    // (2) Pure trivia shift: prepend a comment. Every decl moves; nothing
    //     structural changes.
    SourceId sid = db_get_file_source(&s, fid);
    const char *e2 = "// shifted\nfoo :: 1\nbar :: 2\nbaz :: 3\n";
    assert(db_set_source_text(&s, sid, e2, strlen(e2)));

    db_request_begin(&s, db_current_revision(&s));
    FileArray items2 = db_query_namespace_items(&s, ns);
    Fingerprint ifp2 = db_slot_fingerprint(&s, QUERY_NAMESPACE_ITEMS, (uint64_t)ns.idx);
    AstId bar_id2 = item_ast_id(items2, bar);
    TopLevelEntry e_bar2 = db_query_top_level_entry(&s, ns, bar);
    Fingerprint  tfp2 = db_slot_fingerprint(&s, QUERY_TOP_LEVEL_ENTRY,
                                            ((uint64_t)ns.idx << 32) | (uint32_t)bar.idx);
    uint32_t bar_start2 = e_bar2.node_ptr.range.start;
    db_request_end(&s);

    assert(ifp2 == ifp1 && "index fp stable across a pure trivia shift (firewall)");
    assert(bar_id2.idx == bar_id1.idx && "AstId stable across the shift");
    assert(bar_start2 != bar_start1 && "node_ptr is current (decl moved with the shift)");
    assert(tfp2 == tfp1 && "top_level_entry fp stable across the shift");

    // (3) Content edit: change foo's value → index fp changes.
    const char *e3 = "// shifted\nfoo :: 999999\nbar :: 2\nbaz :: 3\n";
    assert(db_set_source_text(&s, sid, e3, strlen(e3)));
    db_request_begin(&s, db_current_revision(&s));
    (void)db_query_namespace_items(&s, ns);
    Fingerprint ifp3 = db_slot_fingerprint(&s, QUERY_NAMESPACE_ITEMS, (uint64_t)ns.idx);
    db_request_end(&s);
    assert(ifp3 != ifp2 && "content edit changes the index fp");

    // (4) Rename bar → qux: the item's AstId (kind+name) changes.
    const char *e4 = "// shifted\nfoo :: 999999\nqux :: 2\nbaz :: 3\n";
    assert(db_set_source_text(&s, sid, e4, strlen(e4)));
    StrId qux = intern(&s, "qux");
    db_request_begin(&s, db_current_revision(&s));
    FileArray items4 = db_query_namespace_items(&s, ns);
    assert(!items_have(items4, bar) && items_have(items4, qux) && "bar renamed to qux");
    AstId qux_id = item_ast_id(items4, qux);
    db_request_end(&s);
    assert(qux_id.idx != bar_id1.idx && "rename yields a different AstId");

    db_free(&s);
    printf("PASS namespace_items: enumeration + index firewall (trivia-stable fp, "
           "current ptr, stable AstId) + content/rename fp changes\n");
    return 0;
}
