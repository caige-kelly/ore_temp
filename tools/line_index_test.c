// C.0b gate — line_index as a pure query (extracted from file_ast).
//
// Validates: (1) line_index produces correct line-start offsets for \n
// and \r\n endings (matching the lexer's lex_newline semantics); (2)
// position.c (db_byte_offset_at) reads line_index's output correctly;
// (3) it's a proper dep-tracked query — editing source A recomputes
// line_index(A) while unrelated B cache-hits (per-source). KEEP_ZONE, ASan.

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/query/engine.h"
#include "../src/db/workspace/workspace.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// parse-layer wrappers (defined in parse.c; shared header lands in C.1).
extern FileArray db_query_line_index(db_query_ctx *ctx, FileId fid);

static FileId open_file(struct db *s, const char *path, const char *text) {
    SourceId src = workspace_did_open(s, path, strlen(path), text, strlen(text));
    return db_lookup_file_by_source(s, src);
}

int main(void) {
    struct db s;
    db_init(&s);

    // A: 3 LF-terminated lines of 7 bytes each → starts {0,7,14,21}.
    FileId fa = open_file(&s, "/a.ore", "x := 1\ny := 2\nz := 3\n");
    // C: CRLF endings "a\r\nb\r\n" → starts {0,3,6}.
    FileId fc = open_file(&s, "/c.ore", "a\r\nb\r\n");

    db_request_begin(&s, db_current_revision(&s));
    FileArray la = db_query_line_index(&s, fa);
    FileArray lc = db_query_line_index(&s, fc);
    db_request_end(&s);

    const uint32_t *sa = (const uint32_t *)la.data;
    assert(la.count == 4 && "A: 4 line starts");
    assert(sa[0] == 0 && sa[1] == 7 && sa[2] == 14 && sa[3] == 21 &&
           "A: LF offsets correct");

    const uint32_t *sc = (const uint32_t *)lc.data;
    assert(lc.count == 3 && "C: 3 line starts");
    assert(sc[0] == 0 && sc[1] == 3 && sc[2] == 6 && "C: CRLF offsets correct");

    // position.c reads line_index's output: line 1, char 0 → offset 7.
    assert(db_byte_offset_at(&s, fa, 1, 0) == 7 &&
           "db_byte_offset_at reads line_index output");
    assert(db_byte_offset_at(&s, fa, 2, 0) == 14 && "line 2 start");

    uint64_t a_crev1 = db_slot_computed_rev(&s, QUERY_LINE_INDEX, fa.idx);
    uint64_t c_crev1 = db_slot_computed_rev(&s, QUERY_LINE_INDEX, fc.idx);

    // Edit A (changes line count) → line_index(A) recomputes; C frozen.
    SourceId sa_id = db_get_file_source(&s, fa);
    const char *a2 = "x := 1\n";  // now 1 line
    assert(db_set_source_text(&s, sa_id, a2, strlen(a2)));

    db_request_begin(&s, db_current_revision(&s));
    FileArray la2 = db_query_line_index(&s, fa);
    (void)db_query_line_index(&s, fc);
    db_request_end(&s);

    assert(db_slot_computed_rev(&s, QUERY_LINE_INDEX, fa.idx) > a_crev1 &&
           "edited A: line_index recomputed");
    assert(la2.count == 2 && "A after edit: 2 line starts (one line + trailing)");
    assert(db_slot_computed_rev(&s, QUERY_LINE_INDEX, fc.idx) == c_crev1 &&
           "unrelated C: line_index cache-hit (per-source)");

    db_free(&s);
    printf("PASS line_index: LF+CRLF offsets correct, position.c reads it, "
           "per-source recompute\n");
    return 0;
}
