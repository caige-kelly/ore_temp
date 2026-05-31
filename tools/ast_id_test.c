// Phase P P7.1.7 — FileAstIdMap correctness gate.
//
// Verifies that:
//   1. After parse, every FileAstId in db.files.ast_id_maps resolves
//      back to a real node in the file's red tree whose kind matches
//      the recorded SyntaxNodePtr.kind.
//   2. The rev map (ptr-hash → id) round-trips: looking up a known
//      node's ptr-hash returns the same id the preorder walker assigned.
//   3. A whitespace edit (prepend a blank line — shifts every node's
//      start by N bytes, but doesn't reshape the tree) triggers a
//      rebuild that still resolves every FileAstId. This is the
//      "anchor survives trivia edit" property the rebuild owes us.
//
// KEEP_ZONE, ASan.

#include "../src/db/db.h"
#include "../src/db/diag/ast_id.h"
#include "../src/db/ids/ids.h"
#include "../src/db/query/engine.h"
#include "../src/db/workspace/workspace.h"
#include "../src/syntax/syntax.h"
#include "../src/syntax/syntax_kind.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

extern struct GreenNode *db_query_file_ast(db_query_ctx *, FileId);

static FileId open_file(struct db *s, const char *path, const char *text) {
    SourceId src = workspace_did_open(s, path, strlen(path),
                                      text, strlen(text));
    return db_lookup_file_by_source(s, src);
}

static const FileAstIdMap *map_for(struct db *s, FileId fid) {
    return (const FileAstIdMap *)vec_get(&s->files.ast_id_maps,
                                         file_id_local(fid));
}

static const char *BASE =
    "a :: 1\n"
    "b :: 2\n"
    "c :: 3\n";

int main(void) {
    struct db s;
    db_init(&s);
    FileId fid = open_file(&s, "/f.ore", BASE);
    SourceId sid = db_get_file_source(&s, fid);

    db_request_begin(&s, db_current_revision(&s));
    struct GreenNode *groot = db_query_file_ast(&s, fid);
    assert(groot && "parse succeeded");

    const FileAstIdMap *m = map_for(&s, fid);
    assert(m->ptrs.count > 0 && "FileAstIdMap populated");
    assert(m->rev.count == m->ptrs.count && "rev mirrors ptrs 1:1");

    // (1) Every recorded ptr resolves to a node with matching kind.
    SyntaxTree *tree = syntax_tree_new(groot);
    SyntaxNode *root = syntax_tree_root(tree);
    for (size_t i = 0; i < m->ptrs.count; i++) {
        SyntaxNodePtr p = *(SyntaxNodePtr *)vec_get((Vec *)&m->ptrs, i);
        SyntaxNode *n = syntax_node_ptr_resolve(p, root);
        assert(n && "every FileAstId resolves against the parse's root");
        assert(syntax_node_kind(n) == p.kind &&
               "resolved kind matches the recorded ptr.kind");
        syntax_node_release(n);
    }

    // (2) rev round-trip: known nodes' ptrs hash to ids in the map.
    for (size_t i = 0; i < m->ptrs.count; i++) {
        SyntaxNodePtr p = *(SyntaxNodePtr *)vec_get((Vec *)&m->ptrs, i);
        void *v = hashmap_get(&m->rev, syntax_node_ptr_hash(p));
        assert(v && "ptr-hash is present in rev");
        uint32_t found_id = (uint32_t)((uintptr_t)v - 1);
        assert(found_id == (uint32_t)i &&
               "rev[ptr-hash] returns the preorder index");
    }
    syntax_node_release(root);
    syntax_tree_free(tree);

    db_request_end(&s);

    // (3) Whitespace prepend: tree shapes are identical, byte offsets
    // shift by 1. Rebuild MUST produce a map where every id still
    // resolves; same count is expected because the file structure is
    // unchanged.
    size_t pre_count = m->ptrs.count;
    const char *SHIFTED =
        "\n"
        "a :: 1\n"
        "b :: 2\n"
        "c :: 3\n";
    assert(db_set_source_text(&s, sid, SHIFTED, strlen(SHIFTED)));
    db_request_begin(&s, db_current_revision(&s));
    struct GreenNode *groot2 = db_query_file_ast(&s, fid);
    assert(groot2);
    const FileAstIdMap *m2 = map_for(&s, fid);
    assert(m2->ptrs.count == pre_count &&
           "same tree shape => same number of recorded ptrs");

    SyntaxTree *t2 = syntax_tree_new(groot2);
    SyntaxNode *r2 = syntax_tree_root(t2);
    for (size_t i = 0; i < m2->ptrs.count; i++) {
        SyntaxNodePtr p = *(SyntaxNodePtr *)vec_get((Vec *)&m2->ptrs, i);
        SyntaxNode *n = syntax_node_ptr_resolve(p, r2);
        assert(n && "rebuilt map resolves every id post-whitespace-edit");
        syntax_node_release(n);
    }
    syntax_node_release(r2);
    syntax_tree_free(t2);
    db_request_end(&s);

    db_free(&s);
    printf("PASS ast_id\n");
    return 0;
}
