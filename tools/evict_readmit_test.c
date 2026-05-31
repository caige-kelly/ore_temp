// Phase L1 — readmission after eviction.
//
// Pre-L1: workspace_did_open / _did_change / _did_change_external on a
// previously-evicted source path reused the old SourceId, called
// db_set_source_text only, and never touched the `evicted` bit or
// re-joined member_files. Downstream queries reading the namespace's
// FILE_SET (namespace_scopes, namespace_type, top_level_entry) saw the
// post-evict empty list → exports permanently lost until LSP restart.
//
// Post-L1: every reopen path calls db_readmit_source, which clears the
// evicted bit and folds the file back into member_files + FILE_SET via
// file_set_add. This test exercises the evict → reopen cycle for all
// three workspace_did_* reopen entrypoints and asserts every observable
// post-condition.

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

static void check_post_readmit(struct db *s, SourceId src, NamespaceId ns,
                               FileId fid, Fingerprint pre_evict_fp,
                               const char *label) {
    assert(!db_get_source_evicted(s, src) && "evicted bit cleared");
    assert(ns_has(s, ns, fid) && "file re-joined member_files");
    assert(fileset_fp(s, ns) == pre_evict_fp &&
           "FILE_SET fp restored to pre-evict value");
    (void)label;
}

int main(void) {
    // -- Case 1: workspace_did_open --
    {
        struct db s;
        db_init(&s);
        FileId fid = open_file(&s, "/a.ore", "x :: 1\n");
        SourceId src = db_get_file_source(&s, fid);
        NamespaceId ns = db_get_file_namespace(&s, fid);
        Fingerprint pre = fileset_fp(&s, ns);
        assert(ns_has(&s, ns, fid) && "baseline: file in namespace");

        // Evict.
        workspace_did_evict_source(&s, "/a.ore", strlen("/a.ore"));
        assert(db_get_source_evicted(&s, src) && "evicted bit set");
        assert(!ns_has(&s, ns, fid) && "file removed from member_files");
        Fingerprint after_evict = fileset_fp(&s, ns);
        assert(after_evict != pre && "FILE_SET fp differs post-evict");

        // Reopen — same path, fresh text. Pre-L1 this would silently
        // leave the namespace empty. SourceId is reused (path is in
        // source_by_path map).
        SourceId reopened = workspace_did_open(&s, "/a.ore", strlen("/a.ore"),
                                               "x :: 2\n", strlen("x :: 2\n"));
        assert(reopened.idx == src.idx && "reused the same SourceId");
        check_post_readmit(&s, src, ns, fid, pre, "did_open");
        db_free(&s);
    }

    // -- Case 2: workspace_did_change --
    {
        struct db s;
        db_init(&s);
        FileId fid = open_file(&s, "/b.ore", "y :: 1\n");
        SourceId src = db_get_file_source(&s, fid);
        NamespaceId ns = db_get_file_namespace(&s, fid);
        Fingerprint pre = fileset_fp(&s, ns);

        workspace_did_evict_source(&s, "/b.ore", strlen("/b.ore"));
        assert(db_get_source_evicted(&s, src));

        workspace_did_change(&s, "/b.ore", strlen("/b.ore"),
                             "y :: 3\n", strlen("y :: 3\n"));
        check_post_readmit(&s, src, ns, fid, pre, "did_change");
        db_free(&s);
    }

    // -- Case 3: workspace_did_change_external (with explicit text) --
    {
        struct db s;
        db_init(&s);
        FileId fid = open_file(&s, "/c.ore", "z :: 1\n");
        SourceId src = db_get_file_source(&s, fid);
        NamespaceId ns = db_get_file_namespace(&s, fid);
        Fingerprint pre = fileset_fp(&s, ns);

        workspace_did_evict_source(&s, "/c.ore", strlen("/c.ore"));
        assert(db_get_source_evicted(&s, src));

        bool ok = workspace_did_change_external(&s, "/c.ore", strlen("/c.ore"),
                                                "z :: 7\n", strlen("z :: 7\n"));
        assert(ok);
        check_post_readmit(&s, src, ns, fid, pre, "did_change_external");
        db_free(&s);
    }

    // -- Case 4: idempotency. db_readmit_source on a non-evicted source
    //    must be a no-op (no spurious FILE_SET bumps, no double-add). --
    {
        struct db s;
        db_init(&s);
        FileId fid = open_file(&s, "/d.ore", "w :: 1\n");
        SourceId src = db_get_file_source(&s, fid);
        NamespaceId ns = db_get_file_namespace(&s, fid);
        Fingerprint pre = fileset_fp(&s, ns);
        uint32_t n_before = 0;
        (void)db_get_namespace_files(&s, ns, &n_before);

        db_readmit_source(&s, src);
        db_readmit_source(&s, src);

        Fingerprint post = fileset_fp(&s, ns);
        uint32_t n_after = 0;
        (void)db_get_namespace_files(&s, ns, &n_after);
        assert(post == pre && "no-op readmit must not change FILE_SET fp");
        assert(n_after == n_before && "no-op readmit must not duplicate");
        db_free(&s);
    }

    printf("PASS evict_readmit: 3 reopen paths revive evicted source "
           "(evicted bit, member_files, FILE_SET fp); no-op idempotency holds\n");
    return 0;
}
