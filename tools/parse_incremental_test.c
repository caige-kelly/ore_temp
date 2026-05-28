// C.1 gate — decl_ast content-hash firewall (contract C2/C20) + the
// first real multi-level dep chain (file_ast → decl_ast).
//
// Two top-level decls A, B. Edit A's value (which grows A and SHIFTS B's
// byte range). Assert:
//   - decl_ast(A)'s fingerprint CHANGED (A's content changed), and
//   - decl_ast(B)'s fingerprint is UNCHANGED even though B moved — the
//     content hash is POSITION-INDEPENDENT, so a sibling edit doesn't
//     cascade into B (the early-cutoff firewall).
// KEEP_ZONE, ASan.

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

// Capture the n-th top-level NODE child's ptr from a parsed file.
static SyntaxNodePtr nth_decl_ptr(struct GreenNode *root, int n) {
    SyntaxNodePtr result = {0};
    SyntaxTree *t = syntax_tree_new(root);
    SyntaxNode *r = syntax_tree_root(t);
    uint32_t nc = syntax_node_num_children(r);
    int found = 0;
    for (uint32_t i = 0; i < nc; i++) {
        SyntaxNode *c = syntax_node_child(r, i);  // NULL for token children
        if (!c) continue;
        if (found == n) {
            result = syntax_node_ptr_new(c);
            syntax_node_release(c);
            break;
        }
        found++;
        syntax_node_release(c);
    }
    syntax_node_release(r);
    syntax_tree_free(t);
    return result;
}

static uint64_t decl_key(FileId fid, SyntaxNodePtr ptr) {
    return ((uint64_t)fid.idx << 32) | (uint32_t)syntax_node_ptr_hash(ptr);
}

int main(void) {
    struct db s;
    db_init(&s);
    FileId fid = open_file(&s, "/a.ore", "A :: 1\nB :: 2\n");

    db_request_begin(&s, db_current_revision(&s));
    struct GreenNode *root1 = db_query_file_ast(&s, fid);
    assert(root1 && "parsed");
    SyntaxNodePtr a1 = nth_decl_ptr(root1, 0);
    SyntaxNodePtr b1 = nth_decl_ptr(root1, 1);
    assert(a1.kind != SYNTAX_KIND_NONE && b1.kind != SYNTAX_KIND_NONE &&
           "two top-level decls found");
    (void)db_query_decl_ast(&s, fid, a1);
    (void)db_query_decl_ast(&s, fid, b1);
    Fingerprint a_fp1 = db_slot_fingerprint(&s, QUERY_DECL_AST, decl_key(fid, a1));
    Fingerprint b_fp1 = db_slot_fingerprint(&s, QUERY_DECL_AST, decl_key(fid, b1));
    db_request_end(&s);

    assert(a_fp1 != FINGERPRINT_NONE && b_fp1 != FINGERPRINT_NONE &&
           "decl_ast produced content fingerprints");

    // Edit A's value: grows A, shifts B.
    SourceId sid = db_get_file_source(&s, fid);
    const char *e2 = "A :: 999\nB :: 2\n";
    assert(db_set_source_text(&s, sid, e2, strlen(e2)));

    db_request_begin(&s, db_current_revision(&s));
    struct GreenNode *root2 = db_query_file_ast(&s, fid);
    SyntaxNodePtr a2 = nth_decl_ptr(root2, 0);
    SyntaxNodePtr b2 = nth_decl_ptr(root2, 1);
    (void)db_query_decl_ast(&s, fid, a2);
    (void)db_query_decl_ast(&s, fid, b2);
    Fingerprint a_fp2 = db_slot_fingerprint(&s, QUERY_DECL_AST, decl_key(fid, a2));
    Fingerprint b_fp2 = db_slot_fingerprint(&s, QUERY_DECL_AST, decl_key(fid, b2));
    db_request_end(&s);

    // B shifted (its ptr/key changed) but its CONTENT hash is unchanged.
    assert(decl_key(fid, b2) != decl_key(fid, b1) && "B's ptr shifted");
    assert(b_fp2 == b_fp1 &&
           "B's content fingerprint is position-independent (firewall)");
    // A's content changed → fingerprint changed.
    assert(a_fp2 != a_fp1 && "A's content fingerprint changed");

    db_free(&s);
    printf("PASS parse_incremental: decl_ast content hash position-independent "
           "(B stable across shift), A changed\n");
    return 0;
}
