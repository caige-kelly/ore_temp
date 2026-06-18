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
#include <unistd.h>

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
        "B :: pub @import(\"./b.ore\")\ntake :: pub fn() -> i32\n    return B.x\n";
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
        const char *txt = "take :: pub fn() -> i32\n    @nope(0)\n";
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
            "c :: @sizeOf(i32)\nuse :: pub fn() -> i32\n    return c\n";
        FileId fid = open_at(&s, "/sz.ore", txt);
        CheckResult r = check_and_summarize(&s, fid, NULL);
        assert(r.errors == 0 &&
               "@sizeOf(i32) types as comptime_int and coerces into i32");
    }

    // (8) Cross-file generic BODY check — the decisive regression for the
    //     "qualified callee never instantiates" gap. `gen` is generic; its body
    //     errors (`a.nofield`) only once instantiated. Calling it cross-file
    //     (`lib.gen(5)`) must recover the callee semantically (IP_TAG_FN_VALUE),
    //     instantiate, and check the body — so the error surfaces in the
    //     CALLEE's file. Before the fn-value fix this silently typed clean.
    {
        char gl[PATH_MAX], gu[PATH_MAX];
        join_path(&td, "gen_lib.ore", gl, sizeof(gl));
        join_path(&td, "gen_use.ore", gu, sizeof(gu));
        write_file(gl, "gen :: pub fn(a: anytype) -> i32\n"
                       "    _ = a.nofield\n    return 0\n");
        const char *USE = "lib :: pub @import(\"./gen_lib.ore\")\n"
                          "caller :: pub fn() -> i32\n    return lib.gen(5)\n";
        FileId fid_use = open_at(&s, gu, USE);
        (void)check_and_summarize(&s, fid_use, NULL); // instantiates lib.gen
        SourceId src_lib = db_lookup_source_by_path(&s, gl, strlen(gl));
        assert(source_id_valid(src_lib) && "gen_lib registered by import");
        FileId fid_lib = db_lookup_file_by_source(&s, src_lib);
        CheckResult r = check_and_summarize(&s, fid_lib, "field access");
        assert(r.found &&
               "cross-file generic instance checks the body → error surfaces");
    }

    // (9) A clean cross-file generic call (return depends on the param, so it
    //     instantiates) types without errors.
    {
        char gl[PATH_MAX], gu[PATH_MAX];
        join_path(&td, "ok_lib.ore", gl, sizeof(gl));
        join_path(&td, "ok_use.ore", gu, sizeof(gu));
        write_file(gl, "id :: pub fn(a: anytype) -> @TypeOf(a)\n    return a\n");
        const char *USE = "lib :: pub @import(\"./ok_lib.ore\")\n"
                          "caller :: pub fn() -> i32\n    return lib.id(7)\n";
        FileId fid = open_at(&s, gu, USE);
        CheckResult r = check_and_summarize(&s, fid, NULL);
        assert(r.errors == 0 && "clean cross-file generic call types");
    }

    // (10) Cross-file generic via `with` — the gap #1 regression. A generic
    //      head used ONLY via `with lib.cb` must monomorphize (the continuation's
    //      fn-type, with its INFERRED return, fills the `anytype` hole) and
    //      body-check, so a body error surfaces in the callee file. Before the
    //      with-monomorphization fix this silently typed clean (the `with` call
    //      was excluded from mono).
    {
        char wl[PATH_MAX], wu[PATH_MAX];
        join_path(&td, "with_lib.ore", wl, sizeof(wl));
        join_path(&td, "with_use.ore", wu, sizeof(wu));
        write_file(wl, "cb :: pub fn(action: anytype) -> i32\n"
                       "    _ = action.nofield\n    return action()\n");
        const char *USE = "lib :: pub @import(\"./with_lib.ore\")\n"
                          "caller :: pub fn() -> i32\n"
                          "    with lib.cb\n    return 9\n";
        FileId fid_use = open_at(&s, wu, USE);
        (void)check_and_summarize(&s, fid_use, NULL); // instantiates lib.cb
        SourceId src_lib = db_lookup_source_by_path(&s, wl, strlen(wl));
        assert(source_id_valid(src_lib) && "with_lib registered by import");
        FileId fid_lib = db_lookup_file_by_source(&s, src_lib);
        CheckResult r = check_and_summarize(&s, fid_lib, "field access");
        assert(r.found &&
               "cross-file `with`-called generic body-checks → error surfaces");
    }

    // (11) Multi-arg generic via `with f(a)` — locks in MAIN-arm monomorphization
    //      of the FLATTEN form. `with lib.cb2(7)` parses (Slice 6.12) to the flat
    //      call `cb2(7, continuation)`: a GENERIC head (the `action` hole) with
    //      n_args=2. The main arm is arity-agnostic, so the instance keys from
    //      BOTH args and body-checks — the body error on the explicit arg `extra`
    //      surfaces in the callee file. (Replaces the deleted dead flatten arm,
    //      which never monomorphized.)
    {
        char wl[PATH_MAX], wu[PATH_MAX];
        join_path(&td, "with_lib2.ore", wl, sizeof(wl));
        join_path(&td, "with_use2.ore", wu, sizeof(wu));
        write_file(wl, "cb2 :: pub fn(extra: i32, action: anytype) -> i32\n"
                       "    _ = extra.nofield\n    return action()\n");
        const char *USE = "lib :: pub @import(\"./with_lib2.ore\")\n"
                          "caller :: pub fn() -> i32\n"
                          "    with lib.cb2(7)\n    return 9\n";
        FileId fid_use = open_at(&s, wu, USE);
        (void)check_and_summarize(&s, fid_use, NULL); // instantiates lib.cb2
        SourceId src_lib = db_lookup_source_by_path(&s, wl, strlen(wl));
        assert(source_id_valid(src_lib) && "with_lib2 registered by import");
        FileId fid_lib = db_lookup_file_by_source(&s, src_lib);
        CheckResult r = check_and_summarize(&s, fid_lib, "field access");
        assert(r.found &&
               "multi-arg `with f(a)` generic monomorphizes via the main arm");
    }

    tmpdir_rm(&td);
    db_free(&s);
    printf("PASS import: builtin enum lookup; @import types end-to-end "
           "with namespace_type dep edge (add stable, remove fires); "
           "@sizeOf returns comptime_int; unknown-builtin + arg-count diags; "
           "cross-file generic instantiates + body-checks via IP_TAG_FN_VALUE "
           "(error surfaces in callee file; clean call types); cross-file "
           "`with`-called generic monomorphizes + body-checks; multi-arg "
           "`with f(a)` generic monomorphizes via the main arm\n");
    return 0;
}
