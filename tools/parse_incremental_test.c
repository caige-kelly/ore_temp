// C.1 gate (contract C2/C20) — position-independent per-name firewall.
//
// Two top-level decls A, B. Edit A's value (grows A, SHIFTS B's byte
// range). Assert via top_level_entry (the per-name content-firewall handle,
// AstId-keyed):
//   - top_level_entry(B)'s fingerprint is UNCHANGED even though B moved —
//     the structural-hash fp is POSITION-INDEPENDENT, so a sibling edit
//     doesn't cascade into B's downstream (the early-cutoff firewall), and
//   - top_level_entry(A)'s fingerprint CHANGED (A's content changed).
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
    assert(a_fp1 != FINGERPRINT_NONE && b_fp1 != FINGERPRINT_NONE &&
           "both entries produced content fingerprints");

    // Edit A's value: grows A, shifts B.
    SourceId sid = db_get_file_source(&s, fid);
    const char *e2 = "A :: 999\nB :: 2\n";
    assert(db_set_source_text(&s, sid, e2, strlen(e2)));

    db_request_begin(&s, db_current_revision(&s));
    (void)db_query_top_level_entry(&s, ns, A);
    (void)db_query_top_level_entry(&s, ns, B);
    Fingerprint a_fp2 = db_slot_fingerprint(&s, QUERY_TOP_LEVEL_ENTRY, tle_key(ns, A));
    Fingerprint b_fp2 = db_slot_fingerprint(&s, QUERY_TOP_LEVEL_ENTRY, tle_key(ns, B));
    db_request_end(&s);

    assert(b_fp2 == b_fp1 &&
           "B's content fingerprint is position-independent (firewall)");
    assert(a_fp2 != a_fp1 && "A's content fingerprint changed");

    db_free(&s);
    printf("PASS parse_incremental: top_level_entry content hash "
           "position-independent (B stable across shift), A changed\n");
    return 0;
}
