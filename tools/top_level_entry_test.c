// C.1.a gate — TOP_LEVEL_ENTRY: the per-name firewall + FILE_SET dep.
//
// Three properties:
//   1. LOOKUP — top_level_entry(ns, "foo") resolves to foo's decl
//      (a real node_ptr + non-zero fp); a missing name yields an empty
//      entry + FINGERPRINT_NONE.
//   2. SIBLING-EDIT FIREWALL — editing a decl that PRECEDES foo (so foo's
//      byte range shifts) reparses file_ast but leaves foo's entry
//      fingerprint UNCHANGED (position-independent structural hash).
//      Editing foo itself DOES change its fingerprint.
//   3. FILE_SET CORRECTNESS — query "foo" in a namespace that doesn't
//      define it → NOT_FOUND; add a SECOND file to that namespace defining
//      "foo"; re-query → now resolves. This is the case a coarse tier bump
//      alone misses (it would cache-hit the old file set's file_ast deps);
//      the per-namespace FILE_SET input fingerprint is what invalidates it.
// KEEP_ZONE, ASan.

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/query/engine.h"
#include "../src/db/workspace/workspace.h"
#include "../src/syntax/syntax.h"

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

// ---- Part 1 + 2: lookup + sibling-edit firewall ----
static void test_lookup_and_firewall(void) {
    struct db s;
    db_init(&s);
    // `bar` is decl 0 (precedes foo); `foo` is decl 1. Editing bar shifts
    // foo's byte range — the strong position-independence case.
    FileId fid = open_file(&s, "/tle.ore", "bar :: 2\nfoo :: 1\n");
    NamespaceId ns = db_get_file_namespace(&s, fid);

    StrId foo = intern(&s, "foo");
    StrId missing = intern(&s, "nope");

    db_request_begin(&s, db_current_revision(&s));
    TopLevelEntry e_foo = db_query_top_level_entry(&s, ns, foo);
    TopLevelEntry e_missing = db_query_top_level_entry(&s, ns, missing);
    Fingerprint foo_fp1 = db_slot_fingerprint(&s, QUERY_TOP_LEVEL_ENTRY,
                                              tle_key(ns, foo));
    Fingerprint missing_fp = db_slot_fingerprint(&s, QUERY_TOP_LEVEL_ENTRY,
                                                 tle_key(ns, missing));
    db_request_end(&s);

    assert(e_foo.name.idx == foo.idx && "foo resolves to itself");
    assert(e_foo.node_ptr.kind != SYNTAX_KIND_NONE && "foo has a real node_ptr");
    assert(foo_fp1 != FINGERPRINT_NONE && "foo entry has a content fp");
    assert(e_missing.node_ptr.kind == SYNTAX_KIND_NONE && "missing → empty");
    assert(missing_fp == FINGERPRINT_NONE && "missing → FINGERPRINT_NONE");

    // Edit bar (decl 0): grows it, SHIFTS foo down. foo's content is
    // untouched → its entry fp must be stable (the firewall).
    SourceId sid = db_get_file_source(&s, fid);
    const char *e2 = "bar :: 222222\nfoo :: 1\n";
    assert(db_set_source_text(&s, sid, e2, strlen(e2)));

    db_request_begin(&s, db_current_revision(&s));
    TopLevelEntry e_foo2 = db_query_top_level_entry(&s, ns, foo);
    Fingerprint foo_fp2 = db_slot_fingerprint(&s, QUERY_TOP_LEVEL_ENTRY,
                                              tle_key(ns, foo));
    db_request_end(&s);

    assert(e_foo2.node_ptr.range.start != e_foo.node_ptr.range.start &&
           "foo's byte range shifted after the sibling edit");
    assert(foo_fp2 == foo_fp1 &&
           "foo's entry fp is position-independent across a sibling edit");

    // Now edit foo itself → its content changes → fp changes.
    const char *e3 = "bar :: 222222\nfoo :: 99999\n";
    assert(db_set_source_text(&s, sid, e3, strlen(e3)));

    db_request_begin(&s, db_current_revision(&s));
    (void)db_query_top_level_entry(&s, ns, foo);
    Fingerprint foo_fp3 = db_slot_fingerprint(&s, QUERY_TOP_LEVEL_ENTRY,
                                              tle_key(ns, foo));
    db_request_end(&s);

    assert(foo_fp3 != foo_fp2 && "editing foo changes its entry fp");

    db_free(&s);
    printf("PASS top_level_entry: lookup + sibling-edit firewall "
           "(foo stable across bar shift, changes on own edit)\n");
}

// ---- Part 3: FILE_SET file-add correctness ----
static void test_file_set_add(void) {
    struct db s;
    db_init(&s);
    // File A defines only `A`; its namespace does NOT define `foo`.
    FileId fa = open_file(&s, "/p3a.ore", "A :: 1\n");
    NamespaceId ns = db_get_file_namespace(&s, fa);
    StrId foo = intern(&s, "foo");

    db_request_begin(&s, db_current_revision(&s));
    TopLevelEntry before = db_query_top_level_entry(&s, ns, foo);
    db_request_end(&s);
    assert(before.node_ptr.kind == SYNTAX_KIND_NONE &&
           "foo NOT_FOUND before the defining file is added");

    // Add a SECOND file to the SAME namespace, defining `foo`. This folds
    // file B's id into ns's FILE_SET fingerprint (db_create_file), the
    // edge that invalidates the cached NOT_FOUND.
    const char *btext = "foo :: 7\n";
    SourceId sb = db_create_source(&s, "/p3b.ore", strlen("/p3b.ore"),
                                   btext, strlen(btext));
    (void)db_create_file(&s, sb, ns);

    db_request_begin(&s, db_current_revision(&s));
    TopLevelEntry after = db_query_top_level_entry(&s, ns, foo);
    Fingerprint after_fp = db_slot_fingerprint(&s, QUERY_TOP_LEVEL_ENTRY,
                                               tle_key(ns, foo));
    db_request_end(&s);

    assert(after.name.idx == foo.idx && "foo resolves after file B added");
    assert(after.node_ptr.kind != SYNTAX_KIND_NONE && "foo has a real node_ptr");
    assert(after_fp != FINGERPRINT_NONE && "foo entry now has a content fp");

    db_free(&s);
    printf("PASS top_level_entry: FILE_SET correctness "
           "(NOT_FOUND → resolves after adding a file defining the name)\n");
}

int main(void) {
    test_lookup_and_firewall();
    test_file_set_add();
    return 0;
}
