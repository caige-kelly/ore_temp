// D3.2 gate — @import end-to-end + builtin dispatch sanity.
//
//   1. db_builtin_kind_of round-trips for every BUILTIN_LIST entry;
//      an unknown StrId → BUILTIN_KIND_UNKNOWN (D3.2A surface).
//   2. End-to-end: a.ore imports b.ore on the real filesystem; B.x
//      types and the namespace check produces zero errors.
//   3. Editing b to ADD an unrelated member leaves the importer clean
//      (no spurious invalidation; the field lookup still resolves).
//   4. Editing b to REMOVE the referenced member surfaces a "no field
//      'x'" diag in a — proves the infer_body → namespace_type dep edge
//      actually fires.
//   5. Unknown builtin → "unknown builtin @nope" diag.
//   6. @import() (zero args) → "expects 1..1 ... got 0" diag from the
//      dispatcher's metadata check.
//   7. @sizeOf(i32) types as comptime_int and coerces into an i32-typed
//      use site (D3.2b — proves the dispatch path is principled across
//      two distinct builtins, not just @import).
//
// KEEP_ZONE, ASan. @import resolution requires real disk paths
// (realpath()); the test uses a fresh tmpdir and tears it down.

// Pull in POSIX 2008 + glibc extensions for mkdtemp / realpath /
// PATH_MAX. The compiler runs with -std=c23 which hides these without
// a feature-test macro.
#define _GNU_SOURCE

#include "../src/db/db.h"
#include "../src/db/diag/diag.h"
#include "../src/db/ids/ids.h"
#include "../src/db/query/builtins.h"
#include "../src/db/query/engine.h"
#include "../src/db/workspace/workspace.h"
#include "../src/support/data_structure/stringpool.h"

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void db_check_namespace(db_query_ctx *ctx, NamespaceId nsid);

// ---- tmpdir helpers --------------------------------------------------------

typedef struct {
    char dir[PATH_MAX];
} TmpDir;

static void tmpdir_make(TmpDir *t) {
    char tpl[] = "/tmp/ore-import-test-XXXXXX";
    char *r = mkdtemp(tpl);
    assert(r && "mkdtemp succeeded");
    // realpath the result so the path matches what @import resolution
    // canonicalizes to (avoids /tmp -> /private/tmp surprises).
    char *real = realpath(r, NULL);
    assert(real);
    snprintf(t->dir, sizeof(t->dir), "%s", real);
    free(real);
}

static void tmpdir_rm(const TmpDir *t) {
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", t->dir);
    (void)system(cmd);
}

static void join_path(const TmpDir *t, const char *name, char *out,
                      size_t cap) {
    snprintf(out, cap, "%s/%s", t->dir, name);
}

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    assert(f && "fopen for write");
    size_t n = strlen(text);
    assert(fwrite(text, 1, n, f) == n);
    fclose(f);
}

// ---- diag helpers ----------------------------------------------------------
//
// Diag.args point into the producing query's diag arena; they're
// only valid INSIDE the request that produced them. So the combined
// check-and-summarize loop runs everything between
// db_request_begin/end (mirrors the check_test.c pattern).

typedef struct {
    int  errors;
    bool found;     // substr match (when looking for one)
} CheckResult;

static CheckResult check_and_summarize(struct db *s, FileId fid,
                                       const char *substr_or_null) {
    NamespaceId ns = db_get_file_namespace(s, fid);
    db_request_begin(s, db_current_revision(s));
    db_check_namespace(s, ns);

    Vec out;
    vec_init(&out, sizeof(Diag));
    db_collect_diags_for_file(s, fid, &out);
    CheckResult r = {0};
    char buf[1024];
    for (size_t i = 0; i < out.count; i++) {
        Diag *d = (Diag *)vec_get(&out, i);
        if (d->severity == DIAG_ERROR)
            r.errors++;
        if (substr_or_null && !r.found) {
            db_format_diag(s, d, buf, sizeof(buf));
            if (strstr(buf, substr_or_null))
                r.found = true;
        }
    }
    vec_free(&out);
    db_request_end(s);
    return r;
}

// Did_open a path with text; return its FileId.
static FileId open_at(struct db *s, const char *path, const char *text) {
    SourceId src =
        workspace_did_open(s, path, strlen(path), text, strlen(text));
    assert(source_id_valid(src));
    return db_lookup_file_by_source(s, src);
}

int main(void) {
    struct db s;
    db_init(&s);

    // (1) Builtin enum sanity — the D3.2A surface.
    {
        assert(db_builtin_kind_of(&s, s.names.IMPORT) == BUILTIN_IMPORT);
        assert(db_builtin_kind_of(&s, s.names.SIZEOF) == BUILTIN_SIZEOF);
        assert(db_builtin_kind_of(&s, s.names.ALIGNOF) == BUILTIN_ALIGNOF);
        assert(db_builtin_kind_of(&s, s.names.TYPEOF) == BUILTIN_TYPEOF);
        StrId nope = pool_intern(&s.strings, "nope", 4);
        assert(db_builtin_kind_of(&s, nope) == BUILTIN_KIND_UNKNOWN);
    }

    // ---- (2)-(4) need real on-disk files for realpath() in @import. ----
    TmpDir td;
    tmpdir_make(&td);

    char a_path[PATH_MAX], b_path[PATH_MAX];
    join_path(&td, "a.ore", a_path, sizeof(a_path));
    join_path(&td, "b.ore", b_path, sizeof(b_path));

    // pub goes AFTER `::` (it's a value-side modifier — see
    // parse.c:397 and tools/namespace_type_test.c for the convention).
    const char *A =
        "B :: pub @import(\"./b.ore\")\ntake :: pub fn() i32\n    B.x\n";
    const char *B_V1 = "x :: pub 7\n";
    write_file(a_path, A);
    write_file(b_path, B_V1);

    // (2) End-to-end import via LAZY-LOAD. Only `a` is opened explicitly;
    // `b` is admitted to the workspace registry by workspace_resolve_import
    // on first @import resolution INSIDE infer_body's open request. The
    // D3.3a fix (db_create_file_lazy) keeps that path from tripping the
    // "db_input_changed while request open" assert. B.x then types cleanly
    // and b's source becomes addressable via db_lookup_source_by_path.
    FileId fid_a = open_at(&s, a_path, A);
    {
        CheckResult r = check_and_summarize(&s, fid_a, NULL);
        assert(r.errors == 0 &&
               "B.x via @import lazy-load types without errors");
    }

    SourceId src_b = db_lookup_source_by_path(&s, b_path, strlen(b_path));
    assert(source_id_valid(src_b) &&
           "b's source registered by the import resolution (lazy-load)");

    // (3) Add an unrelated member to b — re-check stays clean.
    const char *B_V2 = "x :: pub 7\ny :: pub 9\n";
    assert(db_set_source_text(&s, src_b, B_V2, strlen(B_V2)));
    {
        CheckResult r = check_and_summarize(&s, fid_a, NULL);
        assert(r.errors == 0 &&
               "adding unrelated member to b doesn't break a");
    }

    // (4) Remove the referenced member — the dep edge fires.
    const char *B_V3 = "pub y :: 9\n"; // x gone
    assert(db_set_source_text(&s, src_b, B_V3, strlen(B_V3)));
    {
        CheckResult r = check_and_summarize(&s, fid_a, "'x'");
        assert(r.found &&
               "removing b.x → 'no field x' diag (dep edge fired)");
    }

    // (5) Unknown builtin.
    {
        const char *txt = "take :: pub fn() i32\n    @nope(0)\n";
        FileId fid = open_at(&s, "/unknown.ore", txt);
        CheckResult r = check_and_summarize(&s, fid, "unknown builtin");
        assert(r.found && "@nope → unknown-builtin diag");
    }

    // (6) Arg-count mismatch — dispatcher metadata check.
    {
        const char *txt = "X :: @import()\n";
        FileId fid = open_at(&s, "/argct.ore", txt);
        CheckResult r = check_and_summarize(&s, fid, "got 0");
        assert(r.found && "@import() with no args → arg-count diag");
    }

    // (7) @sizeOf returns comptime_int → coerces into a typed use site.
    {
        const char *txt =
            "c :: @sizeOf(i32)\nuse :: pub fn() i32\n    c\n";
        FileId fid = open_at(&s, "/sz.ore", txt);
        CheckResult r = check_and_summarize(&s, fid, NULL);
        assert(r.errors == 0 &&
               "@sizeOf(i32) types as comptime_int and coerces into i32");
    }

    tmpdir_rm(&td);
    db_free(&s);
    printf("PASS import: builtin enum lookup; @import types end-to-end "
           "with namespace_type dep edge (add stable, remove fires); "
           "@sizeOf returns comptime_int; unknown-builtin + arg-count diags\n");
    return 0;
}
