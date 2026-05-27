// Tier 2 audit-close gate — db_query_node_type router.
//
// The unified node→type router (src/db/query/node_type.c) is the IDE
// hover entry point. For a SyntaxNode at any offset in the file, it:
//   1. Walks parents to find the enclosing top-level decl (DefId).
//   2. Dispatches on the def's kind to the per-decl NodeTypesRange.
//   3. Falls back to the decl's overall type for nodes that aren't
//      in any per-decl range (the decl's own name token, for example).
//
// Without this test, hover would silently regress for one of the
// branches (KIND_FUNCTION body, KIND_FUNCTION signature, KIND_CONSTANT
// value, KIND_STRUCT field, or the fallback path) and we'd only
// notice when LSP users complained.
//
// Fixture: a fn + a const + a struct. We probe specific offsets and
// assert each lands on the right per-decl range.

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/intern_pool/intern_pool.h"
#include "../src/db/query/node_type.h"
#include "../src/db/request/request.h"
#include "../src/sema/sema.h"
#include "../src/syntax/syntax.h"

#include <stdio.h>
#include <string.h>

extern Fingerprint db_query_top_level_index(struct db *s, NamespaceId mod);

// Look up a SyntaxNode at a 0-indexed (line, col) position and query
// its IpIndex via the router. Returns IP_NONE on miss.
static IpIndex type_at(struct db *db, FileId fid, uint32_t line, uint32_t col) {
  uint32_t off = db_byte_offset_at(db, fid, line, col);
  if (off == UINT32_MAX) return IP_NONE;
  SyntaxNode *node = db_node_at_offset(db, fid, off);
  if (!node) return IP_NONE;
  IpIndex t = db_query_node_type(db, fid, node);
  syntax_node_release(node);
  return t;
}

int main(void) {
  struct db db;
  db_init(&db);

  const char *path = "router.ore";
  // Lines (0-indexed):
  //   0: fn_a :: fn(x: i32) i32 { x }
  //   1: K :: 42
  //   2: Point :: struct { p: i32 }
  const char *text = "fn_a :: fn(x: i32) i32 { x }\n"
                     "K :: 42\n"
                     "Point :: struct { p: i32 }\n";

  SourceId src = db_create_source(&db, path, strlen(path), text, strlen(text));
  NamespaceId M = db_create_namespace(&db);
  FileId fid = db_create_file(&db, src, M);

  int ok = 1;

  db_request_begin(&db, 1);
  (void)db_query_top_level_index(&db, M);
  sema_check_module(&db, M);

  // ---- Probe 1: `x` parameter name inside the signature ----
  //   "fn_a :: fn(x: i32) i32 { x }"
  //              ^ col 11 — the SK_PARAM (or its IDENT `x`) lives here.
  // The signature_node_types HashMap was populated with this param's
  // type during fn_signature; the router should find it.
  IpIndex t_param = type_at(&db, fid, /*line=*/0, /*col=*/11);
  if (t_param.v != IP_I32_TYPE.v) {
    fprintf(stderr, "FAIL: type at param `x` = %u (want IP_I32_TYPE=%u)\n",
            t_param.v, IP_I32_TYPE.v);
    ok = 0;
  }

  // ---- Probe 2: `x` reference in body ----
  //   "fn_a :: fn(x: i32) i32 { x }"
  //                              ^ col 25 — the SK_REF_EXPR for x.
  // body_node_types populated this node's type during infer_body.
  IpIndex t_body = type_at(&db, fid, /*line=*/0, /*col=*/25);
  if (t_body.v != IP_I32_TYPE.v) {
    fprintf(stderr, "FAIL: type at body-`x` = %u (want IP_I32_TYPE=%u)\n",
            t_body.v, IP_I32_TYPE.v);
    ok = 0;
  }

  // ---- Probe 3: top-level name `K` — falls back to decl-type ----
  //   "K :: 42"
  //    ^ col 0
  // The router can't find K's name token in any per-decl range (the
  // name IS the decl's own anchor), so it falls through to
  // db_query_type_of_def(K) which returns IP_COMPTIME_INT_TYPE.
  IpIndex t_K = type_at(&db, fid, /*line=*/1, /*col=*/0);
  if (t_K.v != IP_COMPTIME_INT_TYPE.v) {
    fprintf(stderr, "FAIL: type at K = %u (want IP_COMPTIME_INT_TYPE=%u)\n",
            t_K.v, IP_COMPTIME_INT_TYPE.v);
    ok = 0;
  }

  // ---- Probe 4: struct field `p` — looks up via field_node_types ----
  //   "Point :: struct { p: i32 }"
  //                       ^ col 18 — the SK_FIELD `p: i32`.
  IpIndex t_field = type_at(&db, fid, /*line=*/2, /*col=*/18);
  if (t_field.v != IP_I32_TYPE.v) {
    fprintf(stderr, "FAIL: type at field `p` = %u (want IP_I32_TYPE=%u)\n",
            t_field.v, IP_I32_TYPE.v);
    ok = 0;
  }

  db_request_end(&db);
  db_free(&db);

  if (ok) {
    printf("PASS node_type_router: param/body/decl-fallback/field branches "
           "all return correct IpIndex\n");
    return 0;
  }
  printf("FAIL node_type_router\n");
  return 1;
}
