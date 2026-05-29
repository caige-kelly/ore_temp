// D2.5 gate — the check driver (db_check_namespace) + unused-decl warnings.
//
//   1. A type error (unknown type) surfaces end-to-end via
//      db_check_namespace → db_collect_diags_for_file.
//   2. Unused = an unreferenced PRIVATE decl. A referenced decl, a `pub`
//      decl, and `main` are all exempt → exactly one warning, carrying the
//      orphan's name.
//   3. Incrementality — adding/removing a reference flips the warning, and
//      back (the plain pass recomputes from the current dep graph).
//   4. Same-type ref-swap — moving a reference between two i32 decls MOVES
//      the warning. Both decls share a type, so infer_body's node-type fold
//      (its fingerprint) is unchanged across the swap: a fp-memoized usage
//      query would backdate and keep the stale warning, but the plain
//      dep-graph pass reads fresh deps and gets it right.
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

static FileId open_file(struct db *s, const char *path, const char *text) {
    SourceId src = workspace_did_open(s, path, strlen(path), text, strlen(text));
    return db_lookup_file_by_source(s, src);
}
static StrId intern(struct db *s, const char *str) {
    return pool_intern(&s->strings, str, strlen(str));
}

// Run the driver for `fid`'s namespace, then summarize the file's diags:
// error count, warning count, and the names carried by unused warnings
// (arg[0] of the "%S is declared but never used" template). The collected
// Diag.args still point into the producing unit's arena — valid here since
// we read them within the same request, before any recompute.
typedef struct {
    int   errors;
    int   warnings;
    StrId warned[16];
    int   n_warned;
} DiagSummary;

static DiagSummary check_and_collect(struct db *s, FileId fid) {
    NamespaceId ns = db_get_file_namespace(s, fid);
    db_request_begin(s, db_current_revision(s));
    db_check_namespace(s, ns);

    Vec out;
    vec_init(&out, sizeof(Diag));
    db_collect_diags_for_file(s, fid, &out);

    DiagSummary r = {0};
    for (size_t i = 0; i < out.count; i++) {
        Diag *d = (Diag *)vec_get(&out, i);
        if (d->severity == DIAG_ERROR) {
            r.errors++;
        } else if (d->severity == DIAG_WARNING) {
            r.warnings++;
            if (d->n_args >= 1 && d->args &&
                d->args[0].kind == DIAG_ARG_STR_ID && r.n_warned < 16)
                r.warned[r.n_warned++] = d->args[0].str;
        }
    }
    vec_free(&out);
    db_request_end(s);
    return r;
}

static bool warned_for(const DiagSummary *r, StrId name) {
    for (int i = 0; i < r->n_warned; i++)
        if (r->warned[i].idx == name.idx) return true;
    return false;
}

int main(void) {
    struct db s;
    db_init(&s);

    // (1) A type error surfaces: an unknown type in a parameter.
    {
        FileId fid = open_file(&s, "/err.ore",
            "f :: fn(a: Bogus) i32\n    return 0\n");
        DiagSummary r = check_and_collect(&s, fid);
        assert(r.errors >= 1 && "unknown type 'Bogus' → an error diag is collected");
    }

    // (2) Unused = unreferenced private; referenced / pub / main exempt.
    //   main       : exempt by name (type-clean body)
    //   user       : pub → not flagged; references `base`
    //   base       : referenced by user → not flagged
    //   lonely     : private + unreferenced → the single warning
    {
        FileId fid = open_file(&s, "/unused.ore",
            "main :: fn() i32\n    return 0\n"
            "user :: pub base\n"
            "base :: 7\n"
            "lonely :: 9\n");
        DiagSummary r = check_and_collect(&s, fid);
        assert(r.errors == 0 && "clean program → no type errors");
        assert(r.warnings == 1 && "exactly one unused-decl warning");
        assert(warned_for(&r, intern(&s, "lonely")) &&
               "lonely (private, unreferenced) is warned");
        assert(!warned_for(&r, intern(&s, "base")) &&
               "base is referenced → not warned");
        assert(!warned_for(&r, intern(&s, "user")) &&
               "user is pub → not warned");
        assert(!warned_for(&r, intern(&s, "main")) &&
               "main is exempt → not warned");
    }

    // (3) Incrementality: a reference edit flips the warning, and back.
    {
        const char *V1 = "user :: pub base\nbase :: 7\nlonely :: 9\n";
        const char *V2 = "user :: pub base\nbase :: lonely\nlonely :: 9\n";
        FileId fid = open_file(&s, "/incr.ore", V1);
        SourceId sid = db_get_file_source(&s, fid);

        DiagSummary a = check_and_collect(&s, fid);
        assert(a.warnings == 1 && warned_for(&a, intern(&s, "lonely")) &&
               "V1: lonely unreferenced → warned");

        assert(db_set_source_text(&s, sid, V2, strlen(V2)));
        DiagSummary b = check_and_collect(&s, fid);
        assert(b.warnings == 0 &&
               "V2: lonely now referenced by base → warning clears");

        assert(db_set_source_text(&s, sid, V1, strlen(V1)));
        DiagSummary c = check_and_collect(&s, fid);
        assert(c.warnings == 1 && warned_for(&c, intern(&s, "lonely")) &&
               "V1 again: warning returns");
    }

    // (4) Same-type ref-swap: the warning MOVES (the case a fp-memoized
    //     usage query would miss — both foo,bar are comptime_int so
    //     infer_body(main)'s fold/fp is unchanged across the swap).
    {
        const char *V1 = "main :: fn() i32\n    return foo\nfoo :: 7\nbar :: 9\n";
        const char *V2 = "main :: fn() i32\n    return bar\nfoo :: 7\nbar :: 9\n";
        FileId fid = open_file(&s, "/swap.ore", V1);
        SourceId sid = db_get_file_source(&s, fid);

        DiagSummary a = check_and_collect(&s, fid);
        assert(a.warnings == 1 && warned_for(&a, intern(&s, "bar")) &&
               "V1: main refs foo → bar is the orphan");

        assert(db_set_source_text(&s, sid, V2, strlen(V2)));
        DiagSummary b = check_and_collect(&s, fid);
        assert(b.warnings == 1 && warned_for(&b, intern(&s, "foo")) &&
               "V2: main refs bar → warning MOVES to foo (plain pass reads fresh deps)");
    }

    db_free(&s);
    printf("PASS check: type errors surface; unused = unreferenced-private "
           "(pub/main/referenced exempt); incremental ref edits flip warnings; "
           "same-type ref-swap moves the warning\n");
    return 0;
}
