// Phase 0 P0 gap G7 — node_type_router probes for VARIABLE.
//
// Existing node_type_router_test covers KIND_FUNCTION (param + body),
// KIND_CONSTANT (decl-name fallback), KIND_STRUCT (field). This test
// adds the KIND_VARIABLE arm (mirrors CONSTANT but reads from
// variables.value_node_types instead of constants.value_node_types).
//
// KIND_UNION coverage gap intentionally NOT added here:
// node_type.c:58-67 shares the STRUCT/UNION arm but reads from
// `s->structs.field_node_types[row]` even when row is a UNIONS-table
// index. Unions don't have a `field_node_types` SoA column today; the
// read crashes with garbage memory. This is a known bug the rewrite
// must fix (either add the column to UNIONS or refactor the arm). A
// hover-on-union-field test will be added in Phase 8 once the rewrite
// resolves this.
//
// KIND_ENUM, KIND_EFFECT, KIND_HANDLER also fall through to the default
// branch — separate work, not in this gate's scope.

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

  const char *path = "router_ext.ore";
  // Line 0: V := 7
  const char *text = "V := 7\n";

  SourceId src = db_create_source(&db, path, strlen(path), text, strlen(text));
  NamespaceId M = db_create_namespace(&db);
  FileId fid = db_create_file(&db, src, M);

  int ok = 1;

  db_request_begin(&db, 1);
  (void)db_query_top_level_index(&db, M);
  sema_check_module(&db, M);

  // ---- Probe 1: variable name `V` — falls back to decl-type ----
  // "V := 7"
  //  ^ col 0 — the SK_IDENT for V at the decl wrapper.
  // Like KIND_CONSTANT, the router can't find V's name in any per-
  // decl range (the name IS the decl's anchor); it falls through to
  // db_query_type_of_def(V). For `V := 7`, V's declared type is
  // comptime_int (the literal 7).
  IpIndex t_var_name = type_at(&db, fid, /*line=*/0, /*col=*/0);
  if (t_var_name.v != IP_COMPTIME_INT_TYPE.v) {
    fprintf(stderr,
            "FAIL: type at variable name `V` = %u, want "
            "IP_COMPTIME_INT_TYPE=%u (decl-fallback for KIND_VARIABLE)\n",
            t_var_name.v, IP_COMPTIME_INT_TYPE.v);
    ok = 0;
  }

  // ---- Probe 2: variable value expression `7` ----
  // "V := 7"
  //       ^ col 5 — the SK_INT_LIT for `7`.
  // value_node_types[row] holds this node's type (populated by sema
  // during type_of_def for the variable). The router's KIND_VARIABLE
  // arm reads from variables.value_node_types.
  IpIndex t_var_value = type_at(&db, fid, /*line=*/0, /*col=*/5);
  if (t_var_value.v != IP_COMPTIME_INT_TYPE.v) {
    fprintf(stderr,
            "FAIL: type at variable value `7` = %u, want "
            "IP_COMPTIME_INT_TYPE=%u\n",
            t_var_value.v, IP_COMPTIME_INT_TYPE.v);
    ok = 0;
  }

  db_request_end(&db);
  db_free(&db);

  if (ok) {
    printf("PASS node_type_router_extended: KIND_VARIABLE name + value "
           "probes (KIND_UNION deferred — known bug)\n");
    return 0;
  }
  printf("FAIL node_type_router_extended\n");
  return 1;
}
