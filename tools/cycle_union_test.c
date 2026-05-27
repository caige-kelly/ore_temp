// Phase 0 P0 gap G2 — cycle correctness for non-struct composite types.
//
// cycle_struct_test covers struct↔struct cycles. This test confirms the
// wip-publish mechanism (src/sema/type_of_def.c:104-113) ALSO applies to
// UNION decls — the same code path promotes to KIND_UNION when the decl
// kind is SK_UNION_DECL.
//
// Engine contract being locked in:
//   1. Mutually-referential unions via `^` resolve without infinite recursion.
//   2. Both decls classify as KIND_UNION.
//   3. Both types materialize (non-IP_NONE) and reference each other
//      through pointer fields.
//
// Implementation note: ore currently encodes union TYPES as
// IP_TAG_STRUCT_TYPE in the intern pool (the DefKind is KIND_UNION but
// the type-pool layout is shared with structs). This test asserts that
// behavior — if the rewrite changes union storage, this test fails
// loudly.

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

  const char *path = "cyclic_union.ore";
  const char *text = "A :: union { b: ^B }\n"
                     "B :: union { a: ^A }\n";

  SourceId src = db_create_source(&db, path, strlen(path), text, strlen(text));
  NamespaceId M = db_create_namespace(&db);
  FileId fid = db_create_file(&db, src, M);
  (void)fid;

  int ok = 1;

  db_request_begin(&db, 1);
  (void)db_query_top_level_index(&db, M);
  sema_check_module(&db, M);

  ScopeId internal = db_get_namespace_internal_scope(&db, M);
  uint32_t s0 = *(uint32_t *)vec_get(&db.scopes.decl_lo, internal.idx);
  uint32_t s1 = s0 + *(uint32_t *)vec_get(&db.scopes.decl_len, internal.idx);
  if (s1 - s0 != 2) {
    fprintf(stderr, "FAIL: expected 2 top-level decls, got %u\n", s1 - s0);
    db_request_end(&db);
    db_free(&db);
    printf("FAIL cycle_union\n");
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
    printf("FAIL cycle_union\n");
    return 1;
  }

  // Both must classify as KIND_UNION.
  if (db_def_kind(&db, defA) != KIND_UNION) {
    fprintf(stderr, "FAIL: A's kind = %u, want KIND_UNION=%u\n",
            db_def_kind(&db, defA), KIND_UNION);
    ok = 0;
  }
  if (db_def_kind(&db, defB) != KIND_UNION) {
    fprintf(stderr, "FAIL: B's kind = %u, want KIND_UNION=%u\n",
            db_def_kind(&db, defB), KIND_UNION);
    ok = 0;
  }

  // Both types must resolve to non-IP_NONE (cycle didn't break).
  IpIndex tA = db_query_type_of_def(&db, defA);
  IpIndex tB = db_query_type_of_def(&db, defB);
  if (tA.v == IP_NONE.v || tB.v == IP_NONE.v) {
    fprintf(stderr,
            "FAIL: A.type=%u B.type=%u (IP_NONE = cycle broke for unions)\n",
            tA.v, tB.v);
    ok = 0;
  }

  // Both materialize as IP_TAG_STRUCT_TYPE (union shares storage with
  // struct at the intern level; only DefKind disambiguates).
  if (ip_tag(&db.intern, tA) != IP_TAG_STRUCT_TYPE) {
    fprintf(stderr, "FAIL: A's intern tag = %d, want IP_TAG_STRUCT_TYPE=%d "
                    "(unions are typed as STRUCT_TYPE)\n",
            ip_tag(&db.intern, tA), IP_TAG_STRUCT_TYPE);
    ok = 0;
  }
  if (ip_tag(&db.intern, tB) != IP_TAG_STRUCT_TYPE) {
    fprintf(stderr, "FAIL: B's intern tag = %d, want IP_TAG_STRUCT_TYPE=%d\n",
            ip_tag(&db.intern, tB), IP_TAG_STRUCT_TYPE);
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
    printf("PASS cycle_union: A↔B mutual union refs typed via shared "
           "wip-cycle mechanism\n");
    return 0;
  }
  printf("FAIL cycle_union\n");
  return 1;
}
