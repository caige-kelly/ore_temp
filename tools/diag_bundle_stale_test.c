// #12 — Keystroke-sequence stale-state test.
//
// Reproduces the LSP cascade-on-edit bug: when the user edits a file
// through intermediate broken states back to a valid state, the
// per-def diag bundles may hold stale items from the transient
// invalid revisions. The CLI compile path never sees this (every CLI
// invocation is a fresh db); the LSP rides the early-cut path on
// every keystroke.
//
// Test pattern:
//   1. Establish a BASELINE: fresh db, open file with V_final, count
//      diags.
//   2. RESET to a separate db (clean slate), open with V_init.
//   3. Replay the keystroke sequence: each call to workspace_did_change
//      advances one keystroke. Final keystroke = V_final.
//   4. Final-state diag count MUST equal the baseline.
//
// Should FAIL on current code (proves the bug). Will PASS after
// capability-based dep tracking (#12) lands.
//
// KEEP_ZONE, ASan.

#include "../src/db/db.h"
#include "../src/db/diag/diag.h"
#include "../src/db/ids/ids.h"
#include "../src/db/query/engine.h"
#include "../src/db/workspace/workspace.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

extern void db_check_namespace(db_query_ctx *ctx, NamespaceId nsid);
extern struct GreenNode *db_query_file_ast(db_query_ctx *ctx, FileId fid);
extern FileArray db_query_line_index(db_query_ctx *ctx, FileId fid);

typedef struct {
    int errors;
    int warnings;
} Counts;

// Same shape as `compile_file` (src/compiler/compile.c) — TWO requests
// (parse, then check) so dep edges between file_ast and the per-def
// queries fully form. Mirrors what the LSP's `oredb_typecheck` path
// actually does on every didChange.
static Counts compile_and_count(struct db *s, FileId fid) {
    // Parse request.
    db_request_begin(s, db_current_revision(s));
    (void)db_query_file_ast(s, fid);
    db_request_end(s);

    // Sema + diag collection.
    NamespaceId ns = db_get_file_namespace(s, fid);
    db_request_begin(s, db_current_revision(s));
    db_check_namespace(s, ns);
    (void)db_query_line_index(s, fid);
    db_request_end(s);

    Vec out;
    vec_init(&out, sizeof(Diag));
    db_collect_diags_for_file(s, fid, &out);

    Counts r = {0};
    for (size_t i = 0; i < out.count; i++) {
        Diag *d = (Diag *)vec_get(&out, i);
        if (d->severity == DIAG_ERROR) r.errors++;
        else if (d->severity == DIAG_WARNING) r.warnings++;
    }
    vec_free(&out);
    return r;
}

// Open a file with the given initial text and return the FileId.
static FileId open_file(struct db *s, const char *path, const char *text) {
    SourceId src = workspace_did_open(s, path, strlen(path), text, strlen(text));
    return db_lookup_file_by_source(s, src);
}

int main(void) {
    // ============================================================
    // Step 1 — BASELINE: fresh db, V_final, count.
    // ============================================================
    const char *V_FINAL =
        "x :: 1\n"
        "foo :: fn(a: i32) i32\n"
        "    a + 1\n";

    Counts baseline;
    {
        struct db s;
        db_init(&s);
        FileId fid = open_file(&s, "/t.ore", V_FINAL);
        baseline = compile_and_count(&s, fid);
        db_free(&s);
    }

    // ============================================================
    // Step 2/3 — Edit-sequence replay against a single db.
    // ============================================================
    //
    // Keystrokes simulate typing `foo :: fn(a: i32) i32\n    a + 1\n`
    // one character at a time, starting from a single clean decl. Each
    // intermediate state has at least one parse / type error. The
    // final state IS V_final and MUST equal `baseline`.
    const char *keystrokes[] = {
        "x :: 1\n",                            // V_init        — clean
        "x :: 1\nf",                           // partial: bare ident at TL
        "x :: 1\nfo",
        "x :: 1\nfoo",
        "x :: 1\nfoo ",
        "x :: 1\nfoo :",
        "x :: 1\nfoo ::",
        "x :: 1\nfoo :: ",
        "x :: 1\nfoo :: f",
        "x :: 1\nfoo :: fn",
        "x :: 1\nfoo :: fn(",
        "x :: 1\nfoo :: fn(a",
        "x :: 1\nfoo :: fn(a:",
        "x :: 1\nfoo :: fn(a: ",
        "x :: 1\nfoo :: fn(a: i32",
        "x :: 1\nfoo :: fn(a: i32)",
        "x :: 1\nfoo :: fn(a: i32) ",
        "x :: 1\nfoo :: fn(a: i32) i32",
        "x :: 1\nfoo :: fn(a: i32) i32\n",
        "x :: 1\nfoo :: fn(a: i32) i32\n    a",
        "x :: 1\nfoo :: fn(a: i32) i32\n    a +",
        "x :: 1\nfoo :: fn(a: i32) i32\n    a + 1",
        "x :: 1\nfoo :: fn(a: i32) i32\n    a + 1\n", // V_final
    };
    const size_t N = sizeof(keystrokes) / sizeof(keystrokes[0]);

    struct db s;
    db_init(&s);
    FileId fid = open_file(&s, "/t.ore", keystrokes[0]);

    Counts final;
    for (size_t i = 0; i < N; i++) {
        workspace_did_change(&s, "/t.ore", strlen("/t.ore"),
                             keystrokes[i], strlen(keystrokes[i]));
        Counts c = compile_and_count(&s, fid);
        if (i == N - 1) {
            final = c;
        }
    }

    db_free(&s);

    // ============================================================
    // Assert: after the full keystroke sequence ending at V_final,
    // the diag counts MUST match the fresh-db baseline. If the
    // per-def diag bundles held stale items from an intermediate
    // broken state, `final.errors` would exceed `baseline.errors`.
    // ============================================================
    if (final.errors != baseline.errors || final.warnings != baseline.warnings) {
        fprintf(stderr,
                "FAIL diag_bundle_stale: keystroke-sequence final state "
                "diags (%d errors, %d warnings) != baseline (%d errors, %d warnings).\n"
                "Per-def diag bundles are holding stale items across "
                "edit boundaries — dep-tracking is incomplete (#12).\n",
                final.errors, final.warnings,
                baseline.errors, baseline.warnings);
        return 1;
    }

    printf("PASS diag_bundle_stale: keystroke sequence through %zu intermediate "
           "states ends with diags matching the fresh-db baseline "
           "(%d errors, %d warnings)\n",
           N, final.errors, final.warnings);
    return 0;
}
