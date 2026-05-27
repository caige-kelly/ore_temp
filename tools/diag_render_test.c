// Tier 2 audit-close gate — diag rendering + span resolution.
//
// Two layered properties:
//   1. `db_format_diag` substitutes template arguments correctly:
//      `%S` interpolates a StrId, etc. — sema emits "undefined
//      identifier '<name>'" diags by interning the name as a StrId
//      and substituting via the formatter.
//   2. `db_resolve_span` converts a TinySpan (file_id + byte start +
//      byte length) to 1-indexed (line, col_start, col_end) against
//      the file's line_starts table.
//
// Fixture: a file with one undefined identifier. We force a sema run
// that emits the diag, then read it back through the public formatter
// + span resolver.

#include "../src/db/db.h"
#include "../src/db/diag/diag.h"
#include "../src/db/ids/ids.h"
#include "../src/db/request/request.h"
#include "../src/sema/sema.h"

#include <stdio.h>
#include <string.h>

extern Fingerprint db_query_top_level_index(struct db *s, NamespaceId mod);

int main(void) {
  struct db db;
  db_init(&db);

  // Line 1 (1-indexed):  "A :: fn(x: f32) f32 { undefined_name }"
  // The body `undefined_name` is resolved to an unbound identifier —
  // sema emits a diag with a StrId arg and a byte-range anchor.
  const char *path = "diag.ore";
  const char *text = "A :: fn(x: f32) f32 { undefined_name }\n";

  SourceId src = db_create_source(&db, path, strlen(path), text, strlen(text));
  NamespaceId M = db_create_namespace(&db);
  FileId fid = db_create_file(&db, src, M);
  (void)fid;

  int ok = 1;

  db_request_begin(&db, 1);
  (void)db_query_top_level_index(&db, M);
  sema_check_module(&db, M);
  db_request_end(&db);

  // Walk every per-slot diag list, find the first error.
  Diag the_err = {0};
  bool found = false;
  for (size_t i = 1; i < db.diag_lists.count && !found; i++) {
    DiagList *dl = (DiagList *)vec_get(&db.diag_lists, i);
    if (!dl) continue;
    Diag *items = (Diag *)dl->items.data;
    for (size_t j = 0; j < dl->items.count; j++) {
      if (items[j].severity == DIAG_ERROR) {
        the_err = items[j];
        found = true;
        break;
      }
    }
  }
  if (!found) {
    fprintf(stderr, "FAIL: no error diag emitted for undefined identifier\n");
    db_free(&db);
    printf("FAIL diag_render\n");
    return 1;
  }

  // ---- Property 1: db_format_diag interpolates the StrId arg. ----
  // sema's diag template for this case is
  //   "undefined identifier '%S'"
  // so we expect 'undefined_name' to appear in the rendered text.
  char buf[256];
  size_t n = db_format_diag(&db, &the_err, buf, sizeof(buf));
  if (n == 0) {
    fprintf(stderr, "FAIL: db_format_diag returned 0 bytes\n");
    ok = 0;
  }
  if (!strstr(buf, "undefined_name")) {
    fprintf(stderr,
            "FAIL: rendered diag does not mention 'undefined_name': %s\n",
            buf);
    ok = 0;
  }
  if (!strstr(buf, "undefined")) {
    fprintf(stderr,
            "FAIL: rendered diag missing template prefix 'undefined': %s\n",
            buf);
    ok = 0;
  }

  // ---- Property 2: db_resolve_span byte → line:col. ----
  // The undefined identifier starts at byte 22 (0-indexed) on line 1.
  // db_resolve_span returns 1-indexed line/col, so we expect line=1,
  // col_start=23 (1-indexed byte 23 = 0-indexed byte 22).
  ResolvedSpan rs;
  if (!db_resolve_span(&db, the_err.anchor, &rs)) {
    fprintf(stderr, "FAIL: db_resolve_span returned false\n");
    ok = 0;
  } else {
    if (rs.line != 1) {
      fprintf(stderr, "FAIL: resolved line = %u, want 1\n", rs.line);
      ok = 0;
    }
    // The diag anchor points at the SK_REF_EXPR for `undefined_name`.
    // 0-indexed byte offset of 'u' is 22 → 1-indexed col 23.
    if (rs.col_start != 23) {
      fprintf(stderr, "FAIL: resolved col_start = %u, want 23\n",
              rs.col_start);
      ok = 0;
    }
    // col_end is exclusive; the ident is 14 chars long → col_end 37.
    if (rs.col_end != 37) {
      fprintf(stderr, "FAIL: resolved col_end = %u, want 37\n",
              rs.col_end);
      ok = 0;
    }
    if (!rs.path || !strstr(rs.path, "diag.ore")) {
      fprintf(stderr, "FAIL: resolved path = %s, want one containing diag.ore\n",
              rs.path ? rs.path : "(null)");
      ok = 0;
    }
  }

  db_free(&db);

  if (ok) {
    printf("PASS diag_render: %%S interpolation works; "
           "TinySpan→line:col resolves correctly\n");
    return 0;
  }
  printf("FAIL diag_render\n");
  return 1;
}
