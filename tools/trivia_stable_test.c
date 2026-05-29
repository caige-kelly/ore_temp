// C.1 gate (contract C3/C19) — trivia stability via top_level_entry.
//
// Adding a comment / whitespace changes bytes, so file_ast DOES reparse
// (unavoidable pre-incremental-parsing) and decls shift. But each decl's
// top_level_entry fingerprint is a trivia-EXCLUDING structural hash, so a
// trivia-only edit must NOT change any decl's fingerprint — even the decl
// whose leading trivia changed, and despite the shift. So downstream
// (sema) cache-hits across whitespace/comment keystrokes.
// (Was decl_ast-based pre-audit; decl_ast was removed as vestigial — its
// firewall role is top_level_entry's. KEEP_ZONE, ASan.)

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/query/engine.h"
#include "../src/db/workspace/workspace.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

extern TopLevelEntry db_query_top_level_entry(db_query_ctx *ctx,
                                              NamespaceId nsid, StrId name);

static FileId open_file(struct db *s, const char *path, const char *text) {
    SourceId src = workspace_did_open(s, path, strlen(path), text, strlen(text));
    return db_lookup_file_by_source(s, src);
}
static StrId intern(struct db *s, const char *str) {
    return pool_intern(&s->strings, str, strlen(str));
}
static uint64_t tle_key(NamespaceId ns, StrId name) {
    return ((uint64_t)ns.idx << 32) | (uint32_t)name.idx;
}

int main(void) {
    struct db s;
    db_init(&s);
    FileId fid = open_file(&s, "/a.ore", "A :: 1\nB :: 2\n");
    NamespaceId ns = db_get_file_namespace(&s, fid);
    StrId A = intern(&s, "A"), B = intern(&s, "B");

    db_request_begin(&s, db_current_revision(&s));
    (void)db_query_top_level_entry(&s, ns, A);
    (void)db_query_top_level_entry(&s, ns, B);
    Fingerprint a_fp1 = db_slot_fingerprint(&s, QUERY_TOP_LEVEL_ENTRY, tle_key(ns, A));
    Fingerprint b_fp1 = db_slot_fingerprint(&s, QUERY_TOP_LEVEL_ENTRY, tle_key(ns, B));
    db_request_end(&s);
    assert(a_fp1 != FINGERPRINT_NONE && b_fp1 != FINGERPRINT_NONE);

    // Trivia-only edit: insert a comment line between A and B. Bytes change
    // (file reparses) and B shifts down, but no decl's STRUCTURE changed —
    // only trivia.
    SourceId sid = db_get_file_source(&s, fid);
    const char *e2 = "A :: 1\n// just a comment\nB :: 2\n";
    assert(db_set_source_text(&s, sid, e2, strlen(e2)));

    db_request_begin(&s, db_current_revision(&s));
    (void)db_query_top_level_entry(&s, ns, A);
    (void)db_query_top_level_entry(&s, ns, B);
    Fingerprint a_fp2 = db_slot_fingerprint(&s, QUERY_TOP_LEVEL_ENTRY, tle_key(ns, A));
    Fingerprint b_fp2 = db_slot_fingerprint(&s, QUERY_TOP_LEVEL_ENTRY, tle_key(ns, B));
    db_request_end(&s);

    assert(a_fp2 == a_fp1 && "A's structural fp stable across trivia edit (C3)");
    assert(b_fp2 == b_fp1 &&
           "B's structural fp stable despite changed leading trivia + shift (C3)");

    db_free(&s);
    printf("PASS trivia_stable: comment/whitespace edit reparses but leaves "
           "top_level_entry structural fingerprints unchanged (C3)\n");
    return 0;
}
