// D2.2 gate — field/variant/namespace member-pool compaction.
// Each pool strands its old range on every recompute (the decl_pool pattern).
// db_request_end → db_engine_compact → pools_maybe_compact reclaims the
// stranded ranges (mark-live-by-slot-state, copy, rewrite (lo,len), swap).
// With a low threshold we churn each pool, assert its compactor fires, and
// that the surviving (live) range is still correct after the swap.
// KEEP_ZONE, ASan.

#include "../src/db/db.h"
#include "../src/db/ids/ids.h"
#include "../src/db/intern_pool/intern_pool.h"
#include "../src/db/query/engine.h"
#include "../src/db/workspace/workspace.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

extern IpIndex db_query_type_of_def(db_query_ctx *ctx, DefId def);
extern IpIndex db_query_namespace_type(db_query_ctx *ctx, NamespaceId nsid);
extern FileArray db_query_namespace_items(db_query_ctx *ctx, NamespaceId nsid);
extern DefId   db_query_def_identity(db_query_ctx *ctx, NamespaceId nsid, AstId id);

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
    for (uint32_t i = 0; i < items.count; i++)
        if (a[i].name.idx == name.idx) return db_query_def_identity(s, ns, a[i].id);
    return DEF_ID_NONE;
}

int main(void) {
    struct db s;
    db_init(&s);
    s.compact_min_threshold = 2;  // stress: compact early

    // --- aggregate_field_pool: churn a struct's field type --------------------
    FileId sf = open_file(&s, "/s.ore", "S :: struct { x: i32 }\n");
    NamespaceId sns = db_get_file_namespace(&s, sf);
    SourceId ssid = db_get_file_source(&s, sf);
    for (int i = 0; i < 8; i++) {
        // i even → u8, odd → i32; i=0 differs from the initial i32.
        const char *t = (i & 1) ? "S :: struct { x: i32 }\n" : "S :: struct { x: u8 }\n";
        assert(db_set_source_text(&s, ssid, t, strlen(t)));
        db_request_begin(&s, db_current_revision(&s));
        (void)db_query_type_of_def(&s, def_of(&s, sns, "S"));
        db_request_end(&s);  // triggers compaction once past threshold
    }
    assert(s.compact_stats.n_compactions[2] >= 1 && "aggregate pool compacted");
    db_request_begin(&s, db_current_revision(&s));
    DefId sdef = def_of(&s, sns, "S");
    (void)db_query_type_of_def(&s, sdef);
    assert(db_aggregate_field_count(&s, sdef) == 1 && "S still has 1 field post-compact");
    assert(ip_index_eq(db_aggregate_field_type(&s, sdef, intern(&s, "x")), IP_I32_TYPE) &&
           "S.x type survives compaction (last edit i=7 was i32)");
    db_request_end(&s);

    // --- enum_variant_pool: churn an enum's variant value --------------------
    FileId ef = open_file(&s, "/e.ore", "E :: enum { A = 0 }\n");
    NamespaceId ens = db_get_file_namespace(&s, ef);
    SourceId esid = db_get_file_source(&s, ef);
    for (int i = 0; i < 8; i++) {
        char t[64];
        snprintf(t, sizeof t, "E :: enum { A = %d }\n", i + 1);
        assert(db_set_source_text(&s, esid, t, strlen(t)));
        db_request_begin(&s, db_current_revision(&s));
        (void)db_query_type_of_def(&s, def_of(&s, ens, "E"));
        db_request_end(&s);
    }
    assert(s.compact_stats.n_compactions[3] >= 1 && "enum pool compacted");
    db_request_begin(&s, db_current_revision(&s));
    DefId edef = def_of(&s, ens, "E");
    (void)db_query_type_of_def(&s, edef);
    uint32_t nv = 0;
    const EnumVariantEntry *vs = db_enum_variants(&s, edef, &nv);
    assert(nv == 1 && vs && vs[0].value == 8 && "E.A value survives compaction (last was 8)");
    db_request_end(&s);

    // --- namespace_field_pool: churn a namespace's pub set -------------------
    FileId nf = open_file(&s, "/n.ore", "keep :: pub fn() {}\nt :: fn() {}\n");
    NamespaceId nns = db_get_file_namespace(&s, nf);
    SourceId nsid_src = db_get_file_source(&s, nf);
    for (int i = 0; i < 8; i++) {
        // i even → t pub (2 members); i=0 differs from the initial t-private.
        const char *t = (i & 1) ? "keep :: pub fn() {}\nt :: fn() {}\n"
                                : "keep :: pub fn() {}\nt :: pub fn() {}\n";
        assert(db_set_source_text(&s, nsid_src, t, strlen(t)));
        db_request_begin(&s, db_current_revision(&s));
        (void)db_query_namespace_type(&s, nns);
        db_request_end(&s);
    }
    assert(s.compact_stats.n_compactions[4] >= 1 && "namespace pool compacted");
    db_request_begin(&s, db_current_revision(&s));
    (void)db_query_namespace_type(&s, nns);
    // last edit (i=7, odd) had t private → 1 member (keep).
    assert(db_namespace_member_count(&s, nns) == 1 && "ns members survive compaction");
    db_request_end(&s);

    db_free(&s);
    printf("PASS pool_compaction: aggregate/enum/namespace pools compact under "
           "churn; surviving ranges stay correct\n");
    return 0;
}
