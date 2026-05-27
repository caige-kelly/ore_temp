// Tier 1 gate — type cycles via `^` pointer chains.
//
// Mutually-referential structs `A :: struct { b: ^B }; B :: struct { a: ^A }`
// must both materialize without infinite recursion. The wip-publish
// pattern in build_struct_type (src/sema/type_of_def.c) is the
// mechanism: when A is mid-build and references B, B's type_of_def
// runs recursively; that in turn references A and finds A's wip
// IpIndex already published in the def's type cell. The recursion
// bottoms out.
//
// If the wip pattern were broken, this test would either hang or
// produce IP_NONE for one of the two types. We assert both materialize
// AND that each field correctly points to the other's IpIndex.

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/intern_pool/intern_pool.h"
#include "../src/db/query/def_identity.h"
#include "../src/db/query/type_of_def.h"
#include "../src/db/request/request.h"
#include "../src/sema/sema.h"
#include "../src/support/data_structure/stringpool.h"

#include <stdio.h>
#include <string.h>

extern Fingerprint db_query_top_level_index(struct db *s, NamespaceId mod);

int main(void) {
  struct db db;
  db_init(&db);

  const char *path = "cyclic.ore";
  // Use `^` so the layout is finite — the C-equivalent of "tying the knot."
  const char *text = "A :: struct { b: ^B }\n"
                     "B :: struct { a: ^A }\n";

  SourceId src = db_create_source(&db, path, strlen(path), text, strlen(text));
  NamespaceId M = db_create_namespace(&db);
  FileId fid = db_create_file(&db, src, M);
  (void)fid;

  int ok = 1;

  db_request_begin(&db, 1);
  (void)db_query_top_level_index(&db, M);
  sema_check_module(&db, M);

  // Find A and B in the module's internal scope.
  ScopeId internal = db_get_namespace_internal_scope(&db, M);
  uint32_t s0 = *(uint32_t *)vec_get(&db.scopes.decl_lo, internal.idx);
  uint32_t s1 = s0 + *(uint32_t *)vec_get(&db.scopes.decl_len, internal.idx);
  if (s1 - s0 != 2) {
    fprintf(stderr, "FAIL: expected 2 top-level decls, got %u\n", s1 - s0);
    db_request_end(&db);
    db_free(&db);
    printf("FAIL cycle_struct\n");
    return 1;
  }

  DefId defA = DEF_ID_NONE, defB = DEF_ID_NONE;
  for (uint32_t i = s0; i < s1; i++) {
    DeclEntry *de = (DeclEntry *)vec_get(&db.scopes.decl_pool, i);
    DefId def = db_query_def_identity(&db, M, de->node_ptr);
    const char *name = pool_get(&db.strings, de->name);
    if (name && strcmp(name, "A") == 0) defA = def;
    else if (name && strcmp(name, "B") == 0) defB = def;
  }
  if (!def_id_valid(defA) || !def_id_valid(defB)) {
    fprintf(stderr, "FAIL: defA=%u defB=%u (both must be valid)\n",
            defA.idx, defB.idx);
    db_request_end(&db);
    db_free(&db);
    printf("FAIL cycle_struct\n");
    return 1;
  }

  // Both must classify as KIND_STRUCT.
  if (db_def_kind(&db, defA) != KIND_STRUCT) {
    fprintf(stderr, "FAIL: A's kind = %u, want KIND_STRUCT\n",
            db_def_kind(&db, defA));
    ok = 0;
  }
  if (db_def_kind(&db, defB) != KIND_STRUCT) {
    fprintf(stderr, "FAIL: B's kind = %u, want KIND_STRUCT\n",
            db_def_kind(&db, defB));
    ok = 0;
  }

  // Both types must resolve to non-IP_NONE struct types.
  IpIndex tA = db_query_type_of_def(&db, defA);
  IpIndex tB = db_query_type_of_def(&db, defB);
  if (tA.v == IP_NONE.v || tB.v == IP_NONE.v) {
    fprintf(stderr,
            "FAIL: A.type=%u B.type=%u (IP_NONE = cycle broke)\n",
            tA.v, tB.v);
    ok = 0;
  }
  if (ip_tag(&db.intern, tA) != IP_TAG_STRUCT_TYPE) {
    fprintf(stderr, "FAIL: A is not a struct type (tag=%d)\n",
            ip_tag(&db.intern, tA));
    ok = 0;
  }
  if (ip_tag(&db.intern, tB) != IP_TAG_STRUCT_TYPE) {
    fprintf(stderr, "FAIL: B is not a struct type (tag=%d)\n",
            ip_tag(&db.intern, tB));
    ok = 0;
  }

  // Verify the cycle: A's field `b` is ^B, B's field `a` is ^A.
  if (ok) {
    IpKey kA = ip_key(&db.intern, tA);
    IpKey kB = ip_key(&db.intern, tB);

    if (kA.struct_type.n_fields != 1) {
      fprintf(stderr, "FAIL: A has %zu fields, want 1\n",
              kA.struct_type.n_fields);
      ok = 0;
    } else {
      IpIndex bField = kA.struct_type.field_types[0];
      if (ip_tag(&db.intern, bField) != IP_TAG_PTR_TYPE) {
        fprintf(stderr, "FAIL: A.b is not ^... (tag=%d)\n",
                ip_tag(&db.intern, bField));
        ok = 0;
      } else if (ip_key(&db.intern, bField).ptr_type.elem.v != tB.v) {
        fprintf(stderr, "FAIL: A.b points to %u, want B's IpIndex %u\n",
                ip_key(&db.intern, bField).ptr_type.elem.v, tB.v);
        ok = 0;
      }
    }
    if (kB.struct_type.n_fields != 1) {
      fprintf(stderr, "FAIL: B has %zu fields, want 1\n",
              kB.struct_type.n_fields);
      ok = 0;
    } else {
      IpIndex aField = kB.struct_type.field_types[0];
      if (ip_tag(&db.intern, aField) != IP_TAG_PTR_TYPE) {
        fprintf(stderr, "FAIL: B.a is not ^... (tag=%d)\n",
                ip_tag(&db.intern, aField));
        ok = 0;
      } else if (ip_key(&db.intern, aField).ptr_type.elem.v != tA.v) {
        fprintf(stderr, "FAIL: B.a points to %u, want A's IpIndex %u\n",
                ip_key(&db.intern, aField).ptr_type.elem.v, tA.v);
        ok = 0;
      }
    }
  }

  db_request_end(&db);
  db_free(&db);

  if (ok) {
    printf("PASS cycle_struct: A↔B mutual struct refs typed via wip-cycle\n");
    return 0;
  }
  printf("FAIL cycle_struct\n");
  return 1;
}
