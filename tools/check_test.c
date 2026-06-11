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
// For the query-totality regression (C): these are KIND_FUNCTION-only at the
// routing layer; calling them on a non-fn must return empty/NULL, not abort.
extern FileArray          db_query_namespace_items(db_query_ctx *, NamespaceId);
extern DefId              db_query_def_identity(db_query_ctx *, NamespaceId, AstId);
extern const FnSignature *db_query_fn_signature(db_query_ctx *, DefId);
extern NodeTypesRange     db_query_infer_body(db_query_ctx *, DefId);
extern const FnBody      *db_query_body_scopes(db_query_ctx *, DefId);

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
            "f :: fn(a: Bogus) -> i32\n    return 0\n");
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
            "main :: fn() -> i32\n    return 0\n"
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
        const char *V1 = "main :: fn() -> i32\n    return foo\nfoo :: 7\nbar :: 9\n";
        const char *V2 = "main :: fn() -> i32\n    return bar\nfoo :: 7\nbar :: 9\n";
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

    // (5) D — `main` exemption is FUNCTION-only: a non-fn decl named `main` is
    //     not an entrypoint and IS flagged unused.
    {
        FileId fid = open_file(&s, "/nonfnmain.ore", "main :: 42\n");
        DiagSummary r = check_and_collect(&s, fid);
        assert(r.warnings == 1 && warned_for(&r, intern(&s, "main")) &&
               "a non-fn `main` is flagged unused (exemption is fn-only)");
    }

    // (6) Phase P cutover — the unused warnings are decoupled from
    //     NAMESPACE_SCOPES by living in their own column
    //     (db.namespaces.check_diags) maintained solely by the
    //     check driver. The legacy DiagList side store + db_diags_clear
    //     are gone; the structural decoupling is now obvious from the
    //     schema. (Old test case that simulated NAMESPACE_SCOPES
    //     recomputing via db_diags_clear was deleted with the legacy.)

    // (7) C — the fn-only queries are TOTAL: calling them on a non-fn DefId
    //     returns empty/NULL instead of tripping the routing assert.
    {
        FileId fid = open_file(&s, "/nonfn.ore", "k :: 7\n");
        NamespaceId ns = db_get_file_namespace(&s, fid);
        db_request_begin(&s, db_current_revision(&s));
        FileArray items = db_query_namespace_items(&s, ns);
        const NamespaceItem *a = (const NamespaceItem *)items.data;
        DefId k = (items.count > 0) ? db_query_def_identity(&s, ns, a[0].id)
                                    : DEF_ID_NONE;
        assert(k.idx != 0 && "non-fn decl `k` minted a DefId");
        (void)db_query_infer_body(&s, k);  // must not abort (reaching the next
                                           // line is the totality assertion)
        assert(db_query_fn_signature(&s, k) == NULL && "fn_signature(non-fn) → NULL");
        assert(db_query_body_scopes(&s, k) == NULL && "body_scopes(non-fn) → NULL");
        db_request_end(&s);
    }

    // (8) Decoupling — "referenced" no longer requires the reference to TYPE
    //     successfully. `T` is misused as a type (value-not-a-type bails to
    //     poison BEFORE type_of_def(T) is ever called), but the RESOLVE_REF
    //     dep's memoized result still marks T referenced. One error, no
    //     false "T never used".
    {
        FileId fid = open_file(&s, "/valnotype.ore",
            "main :: pub fn() -> i32\n    return 0\n"
            "T :: 42\n"
            "f :: pub fn(a: T) -> i32\n    return 0\n");
        DiagSummary r = check_and_collect(&s, fid);
        assert(r.errors == 1 && "exactly the value-not-a-type error");
        assert(!warned_for(&r, intern(&s, "T")) &&
               "T is referenced (even though the reference failed to type)");
    }

    // (9) An effect referenced ONLY in a row annotation (build_effect_row
    //     stores raw DefIds — no type_of_def on labels) is still "used":
    //     the label's RESOLVE_REF result marks it.
    {
        FileId fid = open_file(&s, "/effrow.ore",
            "main :: pub fn(cb: Fn() <Io> -> void) -> i32\n    return 0\n"
            "Io :: effect\n  ping :: direct() void\n");
        DiagSummary r = check_and_collect(&s, fid);
        assert(r.errors == 0 && "row-annotation-only program is clean");
        assert(!warned_for(&r, intern(&s, "Io")) &&
               "Io referenced via the row label's RESOLVE_REF result");
    }

    // (10) Mutual struct recursion: A is referenced only by B's field. If
    //      typing B re-enters type_of_def(A) while A is RUNNING, the engine's
    //      CYCLE begin records NO dep — but B's frame still holds the
    //      RESOLVE_REF(A) dep, which now marks A.
    {
        FileId fid = open_file(&s, "/mutual.ore",
            "main :: pub fn(b: B) -> i32\n    return 0\n"
            "B :: struct\n    a : ^A\n"
            "A :: struct\n    b : ^B\n");
        DiagSummary r = check_and_collect(&s, fid);
        assert(r.errors == 0 && r.warnings == 0 &&
               "no false 'A never used' in mutual recursion");
    }

    // (11) Generic bodies are skipped in infer_body and checked per-instance;
    //      a decl referenced ONLY inside an instantiated generic body has its
    //      edges on the QUERY_INFER_INSTANCE slot, reached by recursion.
    //      Negative: without an instantiating call the generic body is never
    //      analyzed → helper is genuinely unreferenced and IS flagged.
    {
        FileId fid = open_file(&s, "/genuse.ore",
            "main :: pub fn() -> i32\n    return getv(7)\n"
            "getv :: fn(x: anytype) -> i32\n    _ = x\n    return helper\n"
            "helper :: 9\n");
        DiagSummary r = check_and_collect(&s, fid);
        assert(r.errors == 0 && "instantiated generic program is clean");
        assert(!warned_for(&r, intern(&s, "helper")) &&
               "helper referenced from the generic body via the instance slot");
        assert(!warned_for(&r, intern(&s, "getv")) &&
               "getv referenced from main");

        FileId fid2 = open_file(&s, "/gennouse.ore",
            "main :: pub fn() -> i32\n    return 0\n"
            "getv :: fn(x: anytype) -> i32\n    _ = x\n    return helper\n"
            "helper :: 9\n");
        DiagSummary r2 = check_and_collect(&s, fid2);
        assert(warned_for(&r2, intern(&s, "helper")) &&
               "uninstantiated generic body is never analyzed → helper IS "
               "flagged (documented boundary)");
        assert(warned_for(&r2, intern(&s, "getv")) &&
               "getv itself unreferenced → flagged");
    }

    // W1 — mutable `:=` binding cannot hold a comptime-only type.
    //
    // Ore binding syntax recap:
    //   `name :: v`     untyped const  (comptime values OK)
    //   `name := v`     untyped var    (W1 rejects comptime-only RHS)
    //   `name : T : v`  typed const
    //   `name : T = v`  typed var      (annotation drives narrowing)
    //
    // Cases:
    //   (a) `x := 5` (no annotation, RHS is comptime_int) → REJECT.
    //   (b) `x : usize = 5` (typed var) → OK (annotation narrows).
    //   (c) `x :: 5` (untyped const) → OK (`::` allows comptime values).
    //   (d) `x := 3.14` (comptime_float) → REJECT with comptime_float diag.
    //   (e) `x := y` (concrete-typed RHS) → OK (sanity).
    {
        // (a) bare `var := literal` rejects
        FileId fa = open_file(&s, "/w1a.ore",
            "f :: pub fn() -> i32\n"
            "    x := 5\n"
            "    _ = x\n"
            "    return 0\n");
        NamespaceId na = db_get_file_namespace(&s, fa);
        db_request_begin(&s, db_current_revision(&s));
        db_check_namespace(&s, na);
        Vec va;
        vec_init(&va, sizeof(Diag));
        db_collect_diags_for_file(&s, fa, &va);
        int errors_a = 0;
        bool saw_comptime_int = false, saw_hint = false;
        char buf[512];
        for (size_t i = 0; i < va.count; i++) {
            Diag *d = (Diag *)vec_get(&va, i);
            if (d->severity != DIAG_ERROR) continue;
            errors_a++;
            db_format_diag(&s, d, buf, sizeof(buf));
            if (strstr(buf, "comptime_int")) saw_comptime_int = true;
            if (strstr(buf, "::") || strstr(buf, "annotate"))
                saw_hint = true;
        }
        vec_free(&va);
        db_request_end(&s);
        assert(errors_a == 1 && "W1(a): `x := 5` produces one error");
        assert(saw_comptime_int && "W1(a) diag names the comptime_int type");
        assert(saw_hint && "W1(a) diag suggests annotation OR `::`");
    }
    {
        // (b) typed var `name : T = value` works (annotation narrows)
        FileId fb = open_file(&s, "/w1b.ore",
            "f :: pub fn() -> i32\n"
            "    x : usize = 5\n"
            "    _ = x\n"
            "    return 0\n");
        DiagSummary rb = check_and_collect(&s, fb);
        assert(rb.errors == 0 &&
               "W1(b): `x : usize = 5` types cleanly (annotation narrows)");
    }
    {
        // (c) immutable `::` binding allows comptime_int
        FileId fc = open_file(&s, "/w1c.ore",
            "f :: pub fn() -> i32\n"
            "    x :: 5\n"
            "    _ = x\n"
            "    return 0\n");
        DiagSummary rc = check_and_collect(&s, fc);
        assert(rc.errors == 0 &&
               "W1(c): `x :: 5` types cleanly (`::` allows comptime_int)");
    }
    {
        // (d) `var := <float_literal>` rejects with comptime_float diag
        FileId fd = open_file(&s, "/w1d.ore",
            "f :: pub fn() -> f32\n"
            "    x := 3.14\n"
            "    _ = x\n"
            "    return 0\n");
        NamespaceId nd = db_get_file_namespace(&s, fd);
        db_request_begin(&s, db_current_revision(&s));
        db_check_namespace(&s, nd);
        Vec vd;
        vec_init(&vd, sizeof(Diag));
        db_collect_diags_for_file(&s, fd, &vd);
        bool saw_comptime_float = false;
        char dbuf[512];
        for (size_t i = 0; i < vd.count; i++) {
            Diag *dg = (Diag *)vec_get(&vd, i);
            if (dg->severity != DIAG_ERROR) continue;
            db_format_diag(&s, dg, dbuf, sizeof(dbuf));
            if (strstr(dbuf, "comptime_float"))
                saw_comptime_float = true;
        }
        vec_free(&vd);
        db_request_end(&s);
        assert(saw_comptime_float &&
               "W1(d) diag for `x := 3.14` names comptime_float");
    }
    {
        // (e) concrete-typed RHS works (sanity — not the bug case)
        FileId fe = open_file(&s, "/w1e.ore",
            "f :: pub fn(a: i32) -> i32\n"
            "    x := a\n"
            "    _ = x\n"
            "    return 0\n");
        DiagSummary re = check_and_collect(&s, fe);
        assert(re.errors == 0 &&
               "W1(e): `x := a` (a: i32) types cleanly — RHS is concrete");
    }

    // W3 — pointer-vs-integer comparison emits an actionable diag.
    //
    // `^T == int_lit` and `^T == nil` (non-optional pointer) are common
    // typos. The generic "cannot apply '==' to ..." message is accurate
    // but unhelpful; the polished diag names BOTH likely fixes (deref
    // or make the type optional + use `nil`). This test asserts:
    //   (a) exactly one error fires for the bad comparison
    //   (b) the rendered message contains 'dereference' (the deref hint)
    //   (c) the rendered message contains '?^' (the optional-pointer hint)
    {
        FileId fid = open_file(&s, "/ptr_eq.ore",
            "f :: pub fn(p: ^usize) -> i32\n"
            "    if (p == 0)\n"
            "        return 1\n"
            "    return 0\n");
        NamespaceId ns = db_get_file_namespace(&s, fid);
        db_request_begin(&s, db_current_revision(&s));
        db_check_namespace(&s, ns);

        Vec out;
        vec_init(&out, sizeof(Diag));
        db_collect_diags_for_file(&s, fid, &out);
        int errors = 0;
        bool saw_deref_hint = false;
        bool saw_optional_hint = false;
        char buf[512];
        for (size_t i = 0; i < out.count; i++) {
            Diag *d = (Diag *)vec_get(&out, i);
            if (d->severity != DIAG_ERROR) continue;
            errors++;
            db_format_diag(&s, d, buf, sizeof(buf));
            if (strstr(buf, "dereference")) saw_deref_hint = true;
            if (strstr(buf, "?^"))          saw_optional_hint = true;
        }
        vec_free(&out);
        db_request_end(&s);

        assert(errors == 1 &&
               "W3: `^usize == 0` produces exactly one error diag");
        assert(saw_deref_hint &&
               "W3 diag suggests dereferencing the pointer");
        assert(saw_optional_hint &&
               "W3 diag suggests making the pointer type optional (?^T)");
    }

    db_free(&s);
    printf("PASS check: type errors surface; unused = unreferenced-private "
           "(pub/main/referenced exempt); incremental ref edits flip warnings; "
           "same-type ref-swap moves the warning; referenced is decoupled from "
           "typing success (resolve-ref/def-identity/instance edges); W1: "
           "mutable `:=` rejects comptime-only RHS (comptime_int / "
           "comptime_float) with annotation/`::` hint, annotated and `::` "
           "forms work, concrete RHS works; W3: pointer-vs-integer comparison "
           "emits actionable diag with deref + optional-pointer hints\n");
    return 0;
}
