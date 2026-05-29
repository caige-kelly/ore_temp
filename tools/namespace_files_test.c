// S1 gate — per-namespace file reverse index behind db_get_namespace_files.
//
//   1. Each workspace file is its own namespace containing exactly itself.
//   2. Multi-file namespace: db_create_source + db_create_file into an
//      EXISTING namespace → both files are returned.
//   3. Evicted exclusion: workspace_did_evict_source tombstones a file;
//      the namespace then presents only the survivor (the index keeps the
//      stale entry, the getter filters it at read).
//   4. Empty namespace and the row-0 sentinel → NULL.
// KEEP_ZONE, ASan (exercises the inner-Vec init/append/free lifecycle).

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
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

static uint32_t ns_count(struct db *s, NamespaceId ns) {
    uint32_t n = 0;
    (void)db_get_namespace_files(s, ns, &n);
    return n;
}

int main(void) {
    struct db s;
    db_init(&s);

    // (1) Each workspace file is its own namespace with exactly that file.
    FileId fa = open_file(&s, "/a.ore", "a :: 1\n");
    FileId fb = open_file(&s, "/b.ore", "b :: 2\n");
    NamespaceId nsa = db_get_file_namespace(&s, fa);
    NamespaceId nsb = db_get_file_namespace(&s, fb);
    assert(ns_count(&s, nsa) == 1 && ns_has(&s, nsa, fa) && "nsa = {a}");
    assert(ns_count(&s, nsb) == 1 && ns_has(&s, nsb, fb) && "nsb = {b}");
    assert(!ns_has(&s, nsa, fb) && "nsa does not contain b");

    // (2) Multi-file namespace: append a second file to nsa directly.
    const char *ctext = "c :: 3\n";
    SourceId src2 = db_create_source(&s, "/a2.ore", strlen("/a2.ore"),
                                     ctext, strlen(ctext));
    FileId fa2 = db_create_file(&s, src2, nsa);
    assert(ns_count(&s, nsa) == 2 && ns_has(&s, nsa, fa) && ns_has(&s, nsa, fa2) &&
           "nsa = {a, a2}");

    // (3) Evicted exclusion: evict a's source → nsa presents only a2.
    workspace_did_evict_source(&s, "/a.ore", strlen("/a.ore"));
    assert(!ns_has(&s, nsa, fa) && ns_has(&s, nsa, fa2) &&
           "evicted file excluded, survivor kept");
    assert(ns_count(&s, nsa) == 1 && "nsa = {a2} after evict");

    // (4) Empty namespace + row-0 sentinel → NULL.
    NamespaceId fresh = db_create_namespace(&s);
    uint32_t n = 99;
    assert(db_get_namespace_files(&s, fresh, &n) == NULL && n == 0 &&
           "empty namespace → NULL");
    NamespaceId sentinel = {0};
    n = 99;
    assert(db_get_namespace_files(&s, sentinel, &n) == NULL && n == 0 &&
           "row-0 sentinel → NULL");

    db_free(&s);
    printf("PASS namespace_files: per-namespace reverse index (membership, "
           "multi-file, evicted exclusion, empty/sentinel → NULL)\n");
    return 0;
}
