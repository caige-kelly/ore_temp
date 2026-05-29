// D2.1 gate — type_of_def + fn_signature + resolve_type_expr.
//   1. struct → IPK_STRUCT_TYPE (fields resolve to primitive types).
//   2. self-referential struct (`Node { next: ^Node }`) → resolves via the
//      wip-published type cell on the cycle, no infinite recursion.
//   3. function → IPK_FN_TYPE (params + return resolved).
//   4. typed const (`K : i32 : 0`) → the annotation type IP_I32_TYPE.
//   5. nominal IpIndex STABLE across a sibling edit (top_level_entry firewall
//      → type_of_def cache-hits).
//   6. a field-type edit FLIPS type_of_def's fingerprint.
// KEEP_ZONE, ASan.

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/intern_pool/intern_pool.h"
#include "../src/db/query/engine.h"
#include "../src/db/workspace/workspace.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

extern FileArray db_query_namespace_items(db_query_ctx *ctx, NamespaceId nsid);
extern DefId     db_query_def_identity(db_query_ctx *ctx, NamespaceId nsid, AstId id);
extern IpIndex   db_query_type_of_def(db_query_ctx *ctx, DefId def);

static FileId open_file(struct db *s, const char *path, const char *text) {
    SourceId src = workspace_did_open(s, path, strlen(path), text, strlen(text));
    return db_lookup_file_by_source(s, src);
}
static StrId intern(struct db *s, const char *str) {
    return pool_intern(&s->strings, str, strlen(str));
}
static DefId def_of(struct db *s, NamespaceId ns, const char *nm) {
    StrId name = intern(s, nm);
    FileArray items = db_query_namespace_items(s, ns);
    const NamespaceItem *a = (const NamespaceItem *)items.data;
    AstId id = AST_ID_NONE;
    for (uint32_t i = 0; i < items.count; i++)
        if (a[i].name.idx == name.idx) { id = a[i].id; break; }
    return db_query_def_identity(s, ns, id);
}

int main(void) {
    struct db s;
    db_init(&s);
    FileId fid = open_file(&s, "/t.ore",
        "Point :: struct { x: i32, y: i32 }\n"
        "Node :: struct { next: ^Node }\n"
        "addone :: fn(a: i32) i32 { }\n"
        "K : i32 : 0\n");
    NamespaceId ns = db_get_file_namespace(&s, fid);

    db_request_begin(&s, db_current_revision(&s));
    DefId point = def_of(&s, ns, "Point");
    IpIndex tp = db_query_type_of_def(&s, point);
    assert(ip_index_is_valid(tp) && ip_tag(&s.intern, tp) == IP_TAG_STRUCT_TYPE &&
           "struct → IPK_STRUCT_TYPE");

    IpIndex tn = db_query_type_of_def(&s, def_of(&s, ns, "Node"));
    assert(ip_index_is_valid(tn) && ip_tag(&s.intern, tn) == IP_TAG_STRUCT_TYPE &&
           "self-ref struct resolves (no infinite recursion)");

    IpIndex tf = db_query_type_of_def(&s, def_of(&s, ns, "addone"));
    assert(ip_index_is_valid(tf) && ip_tag(&s.intern, tf) == IP_TAG_FN_TYPE &&
           "fn → IPK_FN_TYPE");

    IpIndex tk = db_query_type_of_def(&s, def_of(&s, ns, "K"));
    assert(ip_index_eq(tk, IP_I32_TYPE) && "typed const → annotation type i32");

    Fingerprint fp_point_1 =
        db_slot_fingerprint(&s, QUERY_TYPE_OF_DECL, (uint64_t)point.idx);
    db_request_end(&s);

    // (5) Sibling edit (K's value) → Point's type_of_def cache-hits → same index.
    SourceId sid = db_get_file_source(&s, fid);
    const char *e2 =
        "Point :: struct { x: i32, y: i32 }\n"
        "Node :: struct { next: ^Node }\n"
        "addone :: fn(a: i32) i32 { }\n"
        "K : i32 : 1\n";
    assert(db_set_source_text(&s, sid, e2, strlen(e2)));
    db_request_begin(&s, db_current_revision(&s));
    IpIndex tp2 = db_query_type_of_def(&s, def_of(&s, ns, "Point"));
    assert(ip_index_eq(tp2, tp) && "nominal IpIndex stable across a sibling edit");
    db_request_end(&s);

    // db_struct_fields recovers the field list (name→type) from the db pool.
    db_request_begin(&s, db_current_revision(&s));
    (void)db_query_type_of_def(&s, def_of(&s, ns, "Point"));
    uint32_t nf = 0;
    const StructFieldEntry *fields = db_struct_fields(&s, point, &nf);
    assert(nf == 2 && fields && "Point has 2 fields in the field pool");
    assert(fields[0].name.idx == intern(&s, "x").idx &&
           ip_index_eq(fields[0].type, IP_I32_TYPE) && "field x: i32");
    assert(fields[1].name.idx == intern(&s, "y").idx &&
           ip_index_eq(fields[1].type, IP_I32_TYPE) && "field y: i32");
    db_request_end(&s);

    // (6) Field-type edit (Point.x i32→u8): the nominal IpIndex is STABLE
    //     (the D2.1b fix — inline identity deduped by zir), but type_of_def's
    //     fp FLIPS (content fold) and db_struct_fields reflects the new type.
    const char *e3 =
        "Point :: struct { x: u8, y: i32 }\n"
        "Node :: struct { next: ^Node }\n"
        "addone :: fn(a: i32) i32 { }\n"
        "K : i32 : 1\n";
    assert(db_set_source_text(&s, sid, e3, strlen(e3)));
    db_request_begin(&s, db_current_revision(&s));
    DefId point3 = def_of(&s, ns, "Point");
    assert(point3.idx == point.idx && "Point DefId stable across field edit");
    IpIndex tp3 = db_query_type_of_def(&s, point3);
    assert(ip_index_eq(tp3, tp) && "nominal IpIndex STABLE across a field-type edit");
    Fingerprint fp_point_3 =
        db_slot_fingerprint(&s, QUERY_TYPE_OF_DECL, (uint64_t)point.idx);
    uint32_t nf3 = 0;
    const StructFieldEntry *fields3 = db_struct_fields(&s, point, &nf3);
    assert(nf3 == 2 && ip_index_eq(fields3[0].type, IP_U8_TYPE) &&
           "field x now u8 in the pool");
    db_request_end(&s);
    assert(fp_point_3 != fp_point_1 && "field-type edit flips type_of_def fp");

    // (7) Fielded → fieldless: the index is STILL stable, the field range is
    //     empty, and no stale fields linger (the bug the redesign fixes).
    const char *e4 =
        "Point :: struct { }\n"
        "Node :: struct { next: ^Node }\n"
        "addone :: fn(a: i32) i32 { }\n"
        "K : i32 : 1\n";
    assert(db_set_source_text(&s, sid, e4, strlen(e4)));
    db_request_begin(&s, db_current_revision(&s));
    IpIndex tp4 = db_query_type_of_def(&s, def_of(&s, ns, "Point"));
    assert(ip_index_eq(tp4, tp) && "fieldless: nominal IpIndex still stable");
    uint32_t nf4 = 99;
    db_struct_fields(&s, point, &nf4);
    assert(nf4 == 0 && "fieldless struct has an empty field range (no stale fields)");
    db_request_end(&s);

    // (8) Mutual recursion A{b:^B} / B{a:^A} — both materialize without
    //     infinite recursion (the self-ref cell anchor handles the cycle),
    //     and each field points to the OTHER's stable struct index.
    FileId fid2 = open_file(&s, "/mutual.ore",
        "A :: struct { b: ^B }\n"
        "B :: struct { a: ^A }\n");
    NamespaceId ns2 = db_get_file_namespace(&s, fid2);
    db_request_begin(&s, db_current_revision(&s));
    DefId a_def = def_of(&s, ns2, "A");
    DefId b_def = def_of(&s, ns2, "B");
    IpIndex ta = db_query_type_of_def(&s, a_def);
    IpIndex tb = db_query_type_of_def(&s, b_def);
    assert(ip_tag(&s.intern, ta) == IP_TAG_STRUCT_TYPE &&
           ip_tag(&s.intern, tb) == IP_TAG_STRUCT_TYPE && "A and B both materialize");
    uint32_t na = 0, nb = 0;
    const StructFieldEntry *fa = db_struct_fields(&s, a_def, &na);
    const StructFieldEntry *fb_ = db_struct_fields(&s, b_def, &nb);
    assert(na == 1 && nb == 1 && "A.b and B.a present");
    IpKey ka = ip_key(&s.intern, fa[0].type);   // ^B
    IpKey kb = ip_key(&s.intern, fb_[0].type);  // ^A
    assert(ka.kind == IPK_PTR_TYPE && ip_index_eq(ka.ptr_type.elem, tb) &&
           "A.b is ^B");
    assert(kb.kind == IPK_PTR_TYPE && ip_index_eq(kb.ptr_type.elem, ta) &&
           "B.a is ^A");
    db_request_end(&s);

    db_free(&s);
    printf("PASS type_of_def: struct/self-ref/fn/typed-const + db_struct_fields; "
           "nominal IpIndex STABLE across sibling/field/fieldless edits; fp flips; "
           "mutual recursion A<->B\n");
    return 0;
}
