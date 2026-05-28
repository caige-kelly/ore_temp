// C.1.b gate — FILE_IMPORTS: @import("path") extraction + fp firewall.
//
//   1. A file with one `@import("std/mem")` yields one FileImport whose
//      path is the interned path (surrounding quotes stripped) and whose
//      site anchors a real node.
//   2. Editing an UNRELATED decl that precedes the import (so the import's
//      byte range shifts) recomputes file_imports but leaves its
//      fingerprint UNCHANGED — the fp folds path StrIds, not positions, so
//      a future module-graph consumer cache-hits across the shift.
//   3. Changing the import PATH changes the fingerprint.
// KEEP_ZONE, ASan.

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/query/engine.h"
#include "../src/db/workspace/workspace.h"
#include "../src/syntax/syntax.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

extern FileArray db_query_file_imports(db_query_ctx *ctx, FileId fid);

static FileId open_file(struct db *s, const char *path, const char *text) {
    SourceId src = workspace_did_open(s, path, strlen(path), text, strlen(text));
    return db_lookup_file_by_source(s, src);
}

int main(void) {
    struct db s;
    db_init(&s);
    // `x` precedes the import so editing x shifts the import's byte range.
    FileId fid = open_file(&s, "/imp.ore",
                           "x :: 1\nstd :: @import(\"std/mem\")\n");
    StrId want = pool_intern(&s.strings, "std/mem", strlen("std/mem"));

    db_request_begin(&s, db_current_revision(&s));
    FileArray imports = db_query_file_imports(&s, fid);
    Fingerprint fp1 = db_slot_fingerprint(&s, QUERY_FILE_IMPORTS,
                                          (uint64_t)fid.idx);
    assert(imports.count == 1 && "one @import site found");
    FileImport *fi = (FileImport *)imports.data;
    assert(fi[0].path.idx == want.idx && "path interned, quotes stripped");
    assert(fi[0].site.kind != SYNTAX_KIND_NONE && "site anchors a real node");
    db_request_end(&s);
    assert(fp1 != FINGERPRINT_NONE && "imports fp non-empty");

    // Edit the unrelated decl x (grows it, shifts the import). The import
    // SET is unchanged → fp stable (the firewall).
    SourceId sid = db_get_file_source(&s, fid);
    const char *e2 = "x :: 999999\nstd :: @import(\"std/mem\")\n";
    assert(db_set_source_text(&s, sid, e2, strlen(e2)));

    db_request_begin(&s, db_current_revision(&s));
    FileArray imports2 = db_query_file_imports(&s, fid);
    Fingerprint fp2 = db_slot_fingerprint(&s, QUERY_FILE_IMPORTS,
                                          (uint64_t)fid.idx);
    assert(imports2.count == 1 && "still one import after the shift");
    db_request_end(&s);
    assert(fp2 == fp1 && "unrelated edit leaves imports fp stable");

    // Change the import path → the import graph shifts → fp changes.
    const char *e3 = "x :: 999999\nstd :: @import(\"std/io\")\n";
    assert(db_set_source_text(&s, sid, e3, strlen(e3)));

    db_request_begin(&s, db_current_revision(&s));
    (void)db_query_file_imports(&s, fid);
    Fingerprint fp3 = db_slot_fingerprint(&s, QUERY_FILE_IMPORTS,
                                          (uint64_t)fid.idx);
    db_request_end(&s);
    assert(fp3 != fp2 && "changing the import path changes the fp");

    db_free(&s);
    printf("PASS file_imports: @import extraction (path+site) + fp stable "
           "across unrelated edit, changes on import edit\n");
    return 0;
}
