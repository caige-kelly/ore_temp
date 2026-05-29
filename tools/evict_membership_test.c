// Phase-8 gate — FILE_SET remove-on-evict. Evicting a file removes it from
// its namespace's member_files index AND recomputes the FILE_SET input
// fingerprint from the surviving membership (db_fp_combine can't subtract,
// so a fold over the survivors reproduces the add-path's value). Proves the
// fp tracks removals, the fold is exact, and an emptied namespace returns
// to the seed. KEEP_ZONE, ASan.

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/query/engine.h"
#include "../src/db/workspace/workspace.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static FileId open_file(struct db *s, const char *path, const char *text) {
    SourceId src = workspace_did_open(s, path, strlen(path), text, strlen(text));
    return db_lookup_file_by_source(s, src);
}

static uint32_t ns_count(struct db *s, NamespaceId ns) {
    uint32_t n = 0;
    (void)db_get_namespace_files(s, ns, &n);
    return n;
}

static bool ns_has(struct db *s, NamespaceId ns, FileId fid) {
    uint32_t n = 0;
    const FileId *fs = db_get_namespace_files(s, ns, &n);
    for (uint32_t i = 0; i < n; i++)
        if (fs[i].idx == fid.idx) return true;
    return false;
}

static Fingerprint fileset_fp(struct db *s, NamespaceId ns) {
    return db_slot_fingerprint(s, QUERY_FILE_SET, (uint64_t)ns.idx);
}

int main(void) {
    struct db s;
    db_init(&s);

    // Multi-file namespace ns = {a, b}.
    FileId a = open_file(&s, "/a.ore", "a :: 1\n");
    NamespaceId ns = db_get_file_namespace(&s, a);
    const char *btext = "b :: 2\n";
    SourceId src_b = db_create_source(&s, "/a2.ore", strlen("/a2.ore"),
                                      btext, strlen(btext));
    FileId b = db_create_file(&s, src_b, ns);
    assert(ns_count(&s, ns) == 2 && "ns = {a, b}");

    Fingerprint fp_before = fileset_fp(&s, ns);

    // Evict a → FILE_SET fp drops a's contribution; ns = {b}.
    workspace_did_evict_source(&s, "/a.ore", strlen("/a.ore"));
    Fingerprint fp_after = fileset_fp(&s, ns);
    assert(fp_after != fp_before &&
           "FILE_SET fp changed on evict (membership shrank)");
    assert(fp_after == db_fp_combine(db_fp_u64(0), db_fp_u64((uint64_t)b.idx)) &&
           "FILE_SET fp == fold of the surviving membership {b}");
    assert(ns_count(&s, ns) == 1 && ns_has(&s, ns, b) && !ns_has(&s, ns, a) &&
           "ns = {b} after evicting a");

    // Evict b → empty namespace; fp returns to the empty-set seed.
    workspace_did_evict_source(&s, "/a2.ore", strlen("/a2.ore"));
    assert(fileset_fp(&s, ns) == db_fp_u64(0) &&
           "empty FILE_SET fp == seed db_fp_u64(0)");
    uint32_t n = 99;
    assert(db_get_namespace_files(&s, ns, &n) == NULL && n == 0 &&
           "empty namespace → NULL");

    db_free(&s);
    printf("PASS evict_membership: FILE_SET fp recomputed on evict "
           "(member drop, fold-of-survivors, empty == seed)\n");
    return 0;
}
