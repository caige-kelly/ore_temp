// C.0 gate — the input→query dependency edge (the substrate that was
// missing). Validates, against the NEW engine, that a derived query
// reading a source input is invalidated PER-SOURCE:
//
//   1. Edit file A  → file_ast(A) recomputes (its SOURCE_TEXT dep fp moved).
//   2. file_ast(B)  → cache-hits (B's source unchanged) — no cascade from
//                     A's edit even though both are DUR_LOW (per-source,
//                     not per-tier).
//   3. Byte-identical re-set of A → no-op (no revision bump, no recompute).
//
// This is the Phase-0 incremental coverage that previously only ran
// against the old engine. It also exercises FILE_AST Vec-routing (the
// SoA slot-sentinel alignment fix) end to end. KEEP_ZONE, ASan.

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/query/engine.h"
#include "../src/db/workspace/workspace.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// Parse-layer wrapper (defined in parse.c; no shared header yet — that
// lands with the C.1 parse.h).
extern struct GreenNode *db_query_file_ast(db_query_ctx *ctx, FileId fid);

static FileId open_file(struct db *s, const char *path, const char *text) {
    SourceId src = workspace_did_open(s, path, strlen(path), text, strlen(text));
    return db_lookup_file_by_source(s, src);
}

static void parse_both(struct db *s, FileId a, FileId b) {
    db_request_begin(s, db_current_revision(s));
    (void)db_query_file_ast(s, a);
    (void)db_query_file_ast(s, b);
    db_request_end(s);
}

int main(void) {
    struct db s;
    db_init(&s);

    FileId fa = open_file(&s, "/a.ore", "A :: 1\n");
    FileId fb = open_file(&s, "/b.ore", "B :: 2\n");

    // Revision 1 — parse both files.
    parse_both(&s, fa, fb);

    Fingerprint fa_fp1 = db_slot_fingerprint(&s, QUERY_FILE_AST, fa.idx);
    Fingerprint fb_fp1 = db_slot_fingerprint(&s, QUERY_FILE_AST, fb.idx);
    uint64_t fa_crev1 = db_slot_computed_rev(&s, QUERY_FILE_AST, fa.idx);
    uint64_t fb_crev1 = db_slot_computed_rev(&s, QUERY_FILE_AST, fb.idx);

    assert(db_slot_state(&s, QUERY_FILE_AST, fa.idx) == QUERY_DONE &&
           "A parsed to DONE (FILE_AST routing works)");
    assert(fa_fp1 != FINGERPRINT_NONE && "A parsed to a real fingerprint");

    // Edit A (real change).
    SourceId sa = db_get_file_source(&s, fa);
    const char *a2 = "A :: 999\n";
    assert(db_set_source_text(&s, sa, a2, strlen(a2)) &&
           "real edit reported as changed");

    // Revision 2 — re-query both.
    QueryStats before = db_query_stats(&s, QUERY_FILE_AST);
    parse_both(&s, fa, fb);
    QueryStats after = db_query_stats(&s, QUERY_FILE_AST);

    // A recomputed (its SOURCE_TEXT dep fingerprint moved).
    assert(db_slot_computed_rev(&s, QUERY_FILE_AST, fa.idx) > fa_crev1 &&
           "edited file A recomputed");
    assert(db_slot_fingerprint(&s, QUERY_FILE_AST, fa.idx) != fa_fp1 &&
           "A's fingerprint changed");

    // B untouched — per-source precision, no cascade from A's edit.
    assert(db_slot_computed_rev(&s, QUERY_FILE_AST, fb.idx) == fb_crev1 &&
           "unedited file B did NOT recompute");
    assert(db_slot_fingerprint(&s, QUERY_FILE_AST, fb.idx) == fb_fp1 &&
           "B's fingerprint unchanged");

    // Exactly one FILE_AST compute (A); B cache-hit.
    assert(after.compute - before.compute == 1 &&
           "only A recomputed this request");
    assert(after.cached_hit - before.cached_hit >= 1 && "B cache-hit");

    // Byte-identical re-set of A is a no-op: no revision bump.
    uint64_t rev_before = db_current_revision(&s);
    assert(!db_set_source_text(&s, sa, a2, strlen(a2)) &&
           "byte-identical edit is a no-op");
    assert(db_current_revision(&s) == rev_before &&
           "no-op edit must not bump the revision");

    db_free(&s);
    printf("PASS input_incremental: edit A → recompute; B → cache-hit "
           "(per-source); byte-identical → no-op\n");
    return 0;
}
