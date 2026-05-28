// C.1 gate (contract C3/C19) — trivia stability.
//
// Adding a comment / whitespace changes bytes, so file_ast DOES reparse
// (unavoidable pre-incremental-parsing). But decl_ast's fingerprint is a
// trivia-EXCLUDING structural hash, so a trivia-only edit must NOT change
// any decl's fingerprint — even the decl whose leading/trailing trivia
// changed, and even though decls shift position. That means downstream
// (sema) cache-hits across whitespace/comment keystrokes. KEEP_ZONE, ASan.

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/query/engine.h"
#include "../src/db/workspace/workspace.h"
#include "../src/syntax/syntax.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

extern struct GreenNode *db_query_file_ast(db_query_ctx *ctx, FileId fid);
extern SyntaxNodePtr     db_query_decl_ast(db_query_ctx *ctx, FileId fid,
                                           SyntaxNodePtr ptr);

static FileId open_file(struct db *s, const char *path, const char *text) {
    SourceId src = workspace_did_open(s, path, strlen(path), text, strlen(text));
    return db_lookup_file_by_source(s, src);
}

static SyntaxNodePtr nth_decl_ptr(struct GreenNode *root, int n) {
    SyntaxNodePtr result = {0};
    SyntaxTree *t = syntax_tree_new(root);
    SyntaxNode *r = syntax_tree_root(t);
    uint32_t nc = syntax_node_num_children(r);
    int found = 0;
    for (uint32_t i = 0; i < nc; i++) {
        SyntaxNode *c = syntax_node_child(r, i);
        if (!c) continue;
        if (found == n) { result = syntax_node_ptr_new(c); syntax_node_release(c); break; }
        found++;
        syntax_node_release(c);
    }
    syntax_node_release(r);
    syntax_tree_free(t);
    return result;
}

static uint64_t dkey(FileId fid, SyntaxNodePtr p) {
    return ((uint64_t)fid.idx << 32) | (uint32_t)syntax_node_ptr_hash(p);
}

int main(void) {
    struct db s;
    db_init(&s);
    FileId fid = open_file(&s, "/a.ore", "A :: 1\nB :: 2\n");

    db_request_begin(&s, db_current_revision(&s));
    struct GreenNode *r1 = db_query_file_ast(&s, fid);
    SyntaxNodePtr a1 = nth_decl_ptr(r1, 0), b1 = nth_decl_ptr(r1, 1);
    (void)db_query_decl_ast(&s, fid, a1);
    (void)db_query_decl_ast(&s, fid, b1);
    Fingerprint a_fp1 = db_slot_fingerprint(&s, QUERY_DECL_AST, dkey(fid, a1));
    Fingerprint b_fp1 = db_slot_fingerprint(&s, QUERY_DECL_AST, dkey(fid, b1));
    db_request_end(&s);
    assert(a_fp1 != FINGERPRINT_NONE && b_fp1 != FINGERPRINT_NONE);

    // Trivia-only edit: insert a comment line between A and B. Bytes
    // change (file reparses) and B shifts down, but no decl's STRUCTURE
    // changed — only trivia.
    SourceId sid = db_get_file_source(&s, fid);
    const char *e2 = "A :: 1\n// just a comment\nB :: 2\n";
    assert(db_set_source_text(&s, sid, e2, strlen(e2)));

    db_request_begin(&s, db_current_revision(&s));
    struct GreenNode *r2 = db_query_file_ast(&s, fid);
    assert(r2 != r1 && "trivia edit still reparses (bytes changed)");
    SyntaxNodePtr a2 = nth_decl_ptr(r2, 0), b2 = nth_decl_ptr(r2, 1);
    (void)db_query_decl_ast(&s, fid, a2);
    (void)db_query_decl_ast(&s, fid, b2);
    Fingerprint a_fp2 = db_slot_fingerprint(&s, QUERY_DECL_AST, dkey(fid, a2));
    Fingerprint b_fp2 = db_slot_fingerprint(&s, QUERY_DECL_AST, dkey(fid, b2));
    db_request_end(&s);

    assert(a_fp2 == a_fp1 && "A's structural fp stable across trivia edit (C3)");
    assert(b_fp2 == b_fp1 &&
           "B's structural fp stable despite changed leading trivia + shift (C3)");

    db_free(&s);
    printf("PASS trivia_stable: comment/whitespace edit reparses but leaves "
           "decl_ast structural fingerprints unchanged (C3)\n");
    return 0;
}
