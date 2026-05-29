// D2.2 gate — namespace_type: the file-as-namespace export type.
//   1. pub top-level decls become members; private decls are excluded.
//   2. the type is IPK_NAMESPACE_TYPE (inline nominal, identity = nsid).
//   3. a body edit leaves the namespace_type fp STABLE (membership-firewalled
//      — member TYPES are lazy, not folded in).
//   4. toggling a decl to `pub` FLIPS the fp and adds the member (meta is in
//      the NAMESPACE_ITEMS membership fp).
// KEEP_ZONE, ASan.

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/intern_pool/intern_pool.h"
#include "../src/db/query/engine.h"
#include "../src/db/workspace/workspace.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

extern IpIndex db_query_namespace_type(db_query_ctx *ctx, NamespaceId nsid);

static FileId open_file(struct db *s, const char *path, const char *text) {
    SourceId src = workspace_did_open(s, path, strlen(path), text, strlen(text));
    return db_lookup_file_by_source(s, src);
}
static StrId intern(struct db *s, const char *str) {
    return pool_intern(&s->strings, str, strlen(str));
}
static bool has_member(struct db *s, NamespaceId ns, const char *nm) {
    StrId name = intern(s, nm);
    uint32_t n = db_namespace_member_count(s, ns);
    for (uint32_t i = 0; i < n; i++)
        if (db_namespace_member_at(s, ns, i).name.idx == name.idx) return true;
    return false;
}

int main(void) {
    struct db s;
    db_init(&s);
    FileId fid = open_file(&s, "/m.ore",
        "Exported :: pub struct { x: i32 }\n"
        "helper :: pub fn() {}\n"
        "secret :: struct { y: i32 }\n");
    NamespaceId ns = db_get_file_namespace(&s, fid);

    db_request_begin(&s, db_current_revision(&s));
    IpIndex nt = db_query_namespace_type(&s, ns);
    assert(ip_index_is_valid(nt) && ip_tag(&s.intern, nt) == IP_TAG_NAMESPACE_TYPE &&
           "namespace_type → IPK_NAMESPACE_TYPE");
    assert(db_namespace_member_count(&s, ns) == 2 && "2 pub members");
    assert(has_member(&s, ns, "Exported") && has_member(&s, ns, "helper") &&
           "pub decls are members");
    assert(!has_member(&s, ns, "secret") && "private decl excluded");
    Fingerprint fp1 = db_slot_fingerprint(&s, QUERY_NAMESPACE_TYPE, (uint64_t)ns.idx);
    db_request_end(&s);

    // (3) Body edit (Exported.x i32→u8) → namespace_type fp STABLE (lazy member
    //     types are not folded; membership unchanged).
    SourceId sid = db_get_file_source(&s, fid);
    const char *e2 =
        "Exported :: pub struct { x: u8 }\n"
        "helper :: pub fn() {}\n"
        "secret :: struct { y: i32 }\n";
    assert(db_set_source_text(&s, sid, e2, strlen(e2)));
    db_request_begin(&s, db_current_revision(&s));
    IpIndex nt2 = db_query_namespace_type(&s, ns);
    assert(ip_index_eq(nt2, nt) && "namespace_type IpIndex stable (inline nsid)");
    Fingerprint fp2 = db_slot_fingerprint(&s, QUERY_NAMESPACE_TYPE, (uint64_t)ns.idx);
    assert(fp2 == fp1 && "body edit leaves namespace_type fp STABLE");
    db_request_end(&s);

    // (4) Toggle `secret` to pub → fp flips, member added.
    const char *e3 =
        "Exported :: pub struct { x: u8 }\n"
        "helper :: pub fn() {}\n"
        "secret :: pub struct { y: i32 }\n";
    assert(db_set_source_text(&s, sid, e3, strlen(e3)));
    db_request_begin(&s, db_current_revision(&s));
    (void)db_query_namespace_type(&s, ns);
    Fingerprint fp3 = db_slot_fingerprint(&s, QUERY_NAMESPACE_TYPE, (uint64_t)ns.idx);
    assert(fp3 != fp1 && "pub toggle flips namespace_type fp");
    assert(db_namespace_member_count(&s, ns) == 3 && has_member(&s, ns, "secret") &&
           "secret is now a pub member");
    db_request_end(&s);

    db_free(&s);
    printf("PASS namespace_type: pub members (private excluded) + IPK_NAMESPACE_TYPE "
           "+ body edit fp-stable + pub-toggle flips fp\n");
    return 0;
}
