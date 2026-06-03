// B1-revised end-to-end gate — virtual files admitted by
// workspace_admit_virtual flow through parse → namespace_type →
// def_identity → type_of_decl exactly like a real user file. No
// synthetic-DefId short-circuits, no compactor whitelists; the
// "virtual file" is a first-class file with a real NamespaceId.
//
// This test exercises TWO paths:
//   1. The db_init-time @target / @build virtual files — verifies
//      s->target_namespace / s->build_namespace populated, query
//      returns IP_TAG_NAMESPACE_TYPE, and the expected members are
//      resolvable. This locks down B1-revised's contract: synthetic
//      Ore source we authored ourselves MUST parse and route through
//      the standard query pipeline without special-casing.
//   2. An ad-hoc virtual file admitted by the test, with const-bind
//      decls — verifies the same path works for arbitrary callers
//      (not just db_init's hardcoded names). Locks in the substrate
//      for future @build outputs / @embedFile / macro expansions
//      called out in workspace.h's SUBSTRATE BOUNDARY doc.
//
// The pre-fix gap that motivates this: virtual_collision_test.c only
// covered the duplicate-name rejection in workspace_admit_virtual.
// The actual cross-system query path (parse → namespace_type →
// member walk via db_namespace_member_at) had never been exercised
// end-to-end in a test — a silent break would have shown up as
// "@target.os doesn't resolve" with no clear signal.
//
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

static StrId intern(struct db *s, const char *str) {
    return pool_intern(&s->strings, str, strlen(str));
}

static bool has_member(struct db *s, NamespaceId ns, const char *nm) {
    StrId name = intern(s, nm);
    uint32_t n = db_namespace_member_count(s, ns);
    for (uint32_t i = 0; i < n; i++)
        if (db_namespace_member_at(s, ns, i).name.idx == name.idx)
            return true;
    return false;
}

int main(void) {
    struct db s;
    db_init(&s);

    // ---- 1. @target / @build admitted at db_init time -------------
    assert(s.target_namespace.idx != 0 &&
           "db_init must populate s->target_namespace via "
           "workspace_admit_virtual (B1-revised)");
    assert(s.build_namespace.idx != 0 &&
           "db_init must populate s->build_namespace via "
           "workspace_admit_virtual (B1-revised)");
    assert(s.target_namespace.idx != s.build_namespace.idx &&
           "@target and @build must be distinct namespaces");

    db_request_begin(&s, db_current_revision(&s));

    IpIndex tnt = db_query_namespace_type(&s, s.target_namespace);
    assert(ip_index_is_valid(tnt) &&
           ip_tag(&s.intern, tnt) == IP_TAG_NAMESPACE_TYPE &&
           "@target namespace_type → IPK_NAMESPACE_TYPE");
    assert(has_member(&s, s.target_namespace, "Os") &&
           "@target declares the Os enum");
    assert(has_member(&s, s.target_namespace, "Arch") &&
           "@target declares the Arch enum");
    assert(has_member(&s, s.target_namespace, "os") &&
           "@target declares the os const binding");
    assert(has_member(&s, s.target_namespace, "arch") &&
           "@target declares the arch const binding");

    IpIndex bnt = db_query_namespace_type(&s, s.build_namespace);
    assert(ip_index_is_valid(bnt) &&
           ip_tag(&s.intern, bnt) == IP_TAG_NAMESPACE_TYPE &&
           "@build namespace_type → IPK_NAMESPACE_TYPE");
    assert(has_member(&s, s.build_namespace, "Mode") &&
           "@build declares the Mode enum");
    assert(has_member(&s, s.build_namespace, "mode") &&
           "@build declares the mode const binding");

    db_request_end(&s);

    // ---- 2. Caller-admitted virtual file routes the same way ------
    // Mirrors the @build outputs / @embedFile / macro-expansion
    // use case from workspace.h's SUBSTRATE BOUNDARY doc.
    const char *vname = "virtual://gen_x.ore";
    const char *vtext =
        "Color :: pub enum\n"
        "    red\n"
        "    blue\n"
        "\n"
        "default :: pub Color.red\n";
    SourceId vsrc = workspace_admit_virtual(&s, vname, strlen(vname),
                                            vtext, strlen(vtext));
    assert(vsrc.idx != SOURCE_ID_NONE.idx &&
           "ad-hoc virtual admit should succeed");
    FileId vfid = db_lookup_file_by_source(&s, vsrc);
    assert(vfid.idx != 0 &&
           "virtual SourceId must map back to a FileId");
    NamespaceId vns = db_get_file_namespace(&s, vfid);
    assert(vns.idx != 0 &&
           "virtual file must have a real NamespaceId (1-file-1-namespace)");

    db_request_begin(&s, db_current_revision(&s));
    IpIndex vnt = db_query_namespace_type(&s, vns);
    assert(ip_index_is_valid(vnt) &&
           ip_tag(&s.intern, vnt) == IP_TAG_NAMESPACE_TYPE &&
           "ad-hoc virtual namespace_type → IPK_NAMESPACE_TYPE");
    assert(has_member(&s, vns, "Color") &&
           "ad-hoc virtual: Color enum is a member");
    assert(has_member(&s, vns, "default") &&
           "ad-hoc virtual: default const binding is a member");
    db_request_end(&s);

    db_free(&s);

    printf("PASS virtual_query: @target+@build admitted at db_init resolve "
           "via the standard query pipeline; ad-hoc virtual files route "
           "the same way (parse \xe2\x86\x92 namespace_type \xe2\x86\x92 "
           "member walk)\n");
    return 0;
}
