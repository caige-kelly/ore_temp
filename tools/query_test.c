#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include "../src/db/db.h"
#include "../src/db/query/query.h"
#include "../src/db/query/invalidate.h"
#include "../src/db/query/collect.h"
#include "../src/db/query/ast_dep.h"
#include "../src/db/request/request.h"
#include "../src/db/workspace/module_info.h"
#include "../src/db/workspace/ast_id_map.h"

/* ---------------------------------------------------------------------- */
/* Full db lifecycle. Every test now exercises StringPool, InternPool,    */
/* HashMap caches, and pre-interned hot names — not just the query bits.  */
/* ---------------------------------------------------------------------- */

static struct db g_db;

static void setup(void)    { db_init(&g_db); }
static void teardown(void) { db_free(&g_db); }

// Allocate a fresh DefId. db_create_def pushes zero rows to every defs
// column, so the slot at &defs.slots_type[def.idx] has state=QUERY_EMPTY
// and kind=0 (which happens to be QUERY_TYPE_OF_DECL — the slot column's
// kind by convention). No explicit slot_init needed for tests.
static DefId alloc_def(void) {
    return db_create_def(&g_db);
}

// Borrow a pointer to the type slot for a DefId. ONLY safe to use before
// any subsequent db_create_def — Vec realloc invalidates this borrow.
// Tests that need stable access use db_locate_slot.
static QuerySlot *slot_for_def(DefId d) {
    return (QuerySlot *)vec_get(&g_db.defs.slots_type, d.idx);
}

/* ---------------------------------------------------------------------- */
/* Tests                                                                  */
/* ---------------------------------------------------------------------- */

static void test_slot_init_state(void) {
    setup();
    DefId d = alloc_def();
    QuerySlot *slot = slot_for_def(d);
    assert(slot->state == QUERY_EMPTY);
    assert(slot->fingerprint == FINGERPRINT_NONE);
    assert(slot->computed_rev == 0);
    assert(slot->verified_rev == 0);
    assert(slot->changed_rev == 0);
    assert(slot->deps == NULL);
    assert(slot->diags == NULL);
    teardown();
}

static void test_begin_compute_on_empty(void) {
    setup();
    DefId d = alloc_def();
    QueryBeginResult r = db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &d);
    assert(r == QUERY_BEGIN_COMPUTE);
    assert(slot_for_def(d)->state == QUERY_RUNNING);
    assert(g_db.query_stack.count == 1);
    QueryFrame *top = db_query_stack_top(&g_db);
    assert(top->kind == QUERY_TYPE_OF_DECL);
    assert(top->key == &d);
    teardown();
}

static void test_succeed_marks_done_and_stores_fp(void) {
    setup();
    DefId d = alloc_def();
    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &d);
    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &d, 0xABCDEFu);
    QuerySlot *slot = slot_for_def(d);
    assert(slot->state == QUERY_DONE);
    assert(slot->fingerprint == 0xABCDEFu);
    assert(slot->computed_rev == 1);
    assert(slot->verified_rev == 1);
    assert(g_db.query_stack.count == 0);
    teardown();
}

static void test_cached_on_done_at_same_rev(void) {
    setup();
    DefId d = alloc_def();
    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &d);
    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &d, 42);

    QueryBeginResult r = db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &d);
    assert(r == QUERY_BEGIN_CACHED);
    assert(g_db.query_stack.count == 0); // No frame pushed on cached path.
    teardown();
}

static void test_cycle_on_running(void) {
    setup();
    DefId d = alloc_def();
    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &d);
    QueryBeginResult r = db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &d);
    assert(r == QUERY_BEGIN_CYCLE);
    teardown();
}

static void test_cancel_propagates(void) {
    setup();
    DefId d = alloc_def();
    db_request_cancel(&g_db);
    QueryBeginResult r = db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &d);
    assert(r == QUERY_BEGIN_CANCELED);
    teardown();
}

static void test_fail_marks_error(void) {
    setup();
    DefId d = alloc_def();
    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &d);
    db_query_fail(&g_db, QUERY_TYPE_OF_DECL, &d);
    QuerySlot *slot = slot_for_def(d);
    assert(slot->state == QUERY_ERROR);
    assert(slot->verified_rev == 1);
    assert(g_db.query_stack.count == 0);
    teardown();
}

// A child query that returns CACHED inside a parent's body should
// record a dep on the parent's frame.
static void test_dep_recorded_on_parent(void) {
    setup();
    DefId b = alloc_def();
    DefId a = alloc_def();

    // Prime B as DONE so the inner begin returns CACHED.
    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &b);
    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &b, 0xB00B);

    // Parent A begins; inside its body it asks for B (cached).
    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &a);
    QueryBeginResult inner = db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &b);
    assert(inner == QUERY_BEGIN_CACHED);

    // A's frame should now hold a dep for B.
    QueryFrame *top = db_query_stack_top(&g_db);
    assert(top->deps != NULL);
    assert(top->deps->count == 1);
    QueryDep *dep = (QueryDep *)vec_get(top->deps, 0);
    assert(dep->kind == QUERY_TYPE_OF_DECL);
    assert(dep->key == &b);
    assert(dep->dep_fp == 0xB00B);

    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &a, 0xAAA);

    // Slot A should now own the same dep list.
    QuerySlot *slot_a = slot_for_def(a);
    assert(slot_a->deps != NULL);
    assert(slot_a->deps->count == 1);
    teardown();
}

// Recompute path: change a dep's fingerprint between revisions and
// verify the parent recomputes (begin returns COMPUTE, not CACHED).
static void test_revalidate_recompute_on_dep_fp_mismatch(void) {
    setup();
    DefId b = alloc_def();
    DefId a = alloc_def();

    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &b);
    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &b, 100);

    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &a);
    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &b); // cached → records dep
    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &a, 200);

    // Simulate edit: bump revision and change B's fingerprint.
    g_db.current_revision = 2;
    slot_for_def(b)->fingerprint = 999;
    slot_for_def(b)->verified_rev = 2;    // pretend B was re-verified
    slot_for_def(b)->computed_rev = 2;

    // A should recompute now.
    QueryBeginResult r = db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &a);
    assert(r == QUERY_BEGIN_COMPUTE);
    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &a, 201);
    teardown();
}

// Stable deps: re-running A at a fresh revision when B's fingerprint
// hasn't changed should hit CACHED (early cutoff via fingerprint match).
static void test_revalidate_skip_when_deps_stable(void) {
    setup();
    DefId b = alloc_def();
    DefId a = alloc_def();

    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &b);
    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &b, 100);

    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &a);
    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &b);
    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &a, 200);

    g_db.current_revision = 2;
    // B was not modified — its fingerprint stays 100, but verified_rev is
    // still 1. db_revalidate should walk, see fp matches, and skip.

    QueryBeginResult r = db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &a);
    assert(r == QUERY_BEGIN_CACHED);
    teardown();
}

// Untracked reads force RECOMPUTE on revalidate regardless of deps.
static void test_revalidate_recompute_when_untracked(void) {
    setup();
    DefId d = alloc_def();

    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &d);
    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &d, 42);
    slot_for_def(d)->has_untracked_read = true;

    g_db.current_revision = 2;
    QueryBeginResult r = db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &d);
    assert(r == QUERY_BEGIN_COMPUTE);
    teardown();
}

// ERROR slots revalidate like DONE — a previously-failed query must be
// retried after an edit that might have fixed the cause.
static void test_error_state_revalidates(void) {
    setup();
    DefId d = alloc_def();

    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &d);
    db_query_fail(&g_db, QUERY_TYPE_OF_DECL, &d);
    assert(slot_for_def(d)->state == QUERY_ERROR);

    // No deps + no untracked + new revision → revalidate walks empty
    // deps, finds nothing wrong, marks verified at new rev, returns
    // SKIP_RECOMPUTE. The ERROR state then surfaces as QUERY_BEGIN_ERROR
    // to the caller (not RECOMPUTE).
    g_db.current_revision = 2;
    QueryBeginResult r = db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &d);
    assert(r == QUERY_BEGIN_ERROR);

    // But if there were a dep whose fp changed, ERROR would recompute.
    // Force-recompute by setting has_untracked_read.
    slot_for_def(d)->has_untracked_read = true;
    slot_for_def(d)->verified_rev = 1; // un-verify so revalidate walks again
    g_db.current_revision = 3;
    r = db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &d);
    assert(r == QUERY_BEGIN_COMPUTE);
    teardown();
}

// Bug-1 regression test: caller never holds a slot pointer; the engine
// re-resolves at every boundary. To prove the fix, allocate enough new
// DefIds DURING a parent's body to force defs.slots_type to realloc its
// malloc buffer. The parent's succeed call must still write to the
// correct (new) slot location, not a dangling one. Run under ASan/UBSan
// for the strongest signal.
static void test_pointer_stability_under_alloc(void) {
    setup();
    DefId parent = alloc_def();
    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &parent);

    // Allocate enough defs to force one or more reallocs of
    // defs.slots_type. The default malloc-Vec doubles, so any sustained
    // growth path will eventually realloc; this gives us many tries.
    for (int i = 0; i < 64; i++) {
        (void)alloc_def();
    }

    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &parent, 0xDEADBEEF);

    // After the dust settles, the parent's slot at its DefId must
    // reflect the succeed call. If we'd written through a dangling
    // pointer this would be UB; under ASan, the test would have crashed
    // before this line.
    QuerySlot *p = slot_for_def(parent);
    assert(p->state == QUERY_DONE);
    assert(p->fingerprint == 0xDEADBEEF);
    teardown();
}

// Bug-2 regression test: the deps Vec object pointer should be reused
// across recomputes (zero arena leak per recompute cycle).
static void test_deps_vec_reused_across_recomputes(void) {
    setup();
    DefId b = alloc_def();
    DefId a = alloc_def();

    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &b);
    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &b, 100);

    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &a);
    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &b); // records dep
    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &a, 200);

    Vec *deps_first = slot_for_def(a)->deps;
    assert(deps_first != NULL);

    // Force recompute by mutating B's fingerprint, then re-run A.
    g_db.current_revision = 2;
    slot_for_def(b)->fingerprint = 999;
    slot_for_def(b)->verified_rev = 2;
    slot_for_def(b)->computed_rev = 2;

    QueryBeginResult r = db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &a);
    assert(r == QUERY_BEGIN_COMPUTE);
    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &b); // re-records dep
    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &a, 201);

    Vec *deps_second = slot_for_def(a)->deps;
    assert(deps_second == deps_first &&
           "deps Vec object must be the same pointer across recomputes");

    // And the malloc buffer should be reused too — count is back to 1
    // (one dep on B), not appended to the prior cycle's deps.
    assert(deps_second->count == 1);
    teardown();
}

/* ---------------------------------------------------------------------- */
/* Lifecycle smoke tests                                                  */
/* ---------------------------------------------------------------------- */

static void test_lifecycle_pre_interned_names(void) {
    setup();

    // Builtin dispatch names.
    assert(g_db.names.sizeOf.idx != 0);
    assert(g_db.names.alignOf.idx != 0);
    assert(g_db.names.TypeOf.idx != 0);
    assert(g_db.names.intCast.idx != 0);
    assert(g_db.names.typeName.idx != 0);
    assert(g_db.names.sizeOf.idx   != g_db.names.alignOf.idx);
    assert(g_db.names.sizeOf.idx   != g_db.names.TypeOf.idx);
    assert(g_db.names.alignOf.idx  != g_db.names.TypeOf.idx);
    assert(g_db.names.intCast.idx  != g_db.names.typeName.idx);

    // Contextual keywords.
    assert(g_db.names.val.idx != 0);
    assert(g_db.names.final.idx != 0);
    assert(g_db.names.raw.idx != 0);
    assert(g_db.names.ctl.idx != 0);
    assert(g_db.names.override.idx != 0);
    assert(g_db.names.named.idx != 0);
    assert(g_db.names.in.idx != 0);
    assert(g_db.names.scoped.idx != 0);
    assert(g_db.names.linear.idx != 0);

    // Spot-check distinctness — same-prefix names ("raw" vs no-prefix,
    // "final" vs "named") shouldn't collide.
    assert(g_db.names.val.idx      != g_db.names.final.idx);
    assert(g_db.names.raw.idx      != g_db.names.ctl.idx);
    assert(g_db.names.override.idx != g_db.names.named.idx);
    assert(g_db.names.scoped.idx   != g_db.names.linear.idx);

    // Round-trip: interning the same string returns the same StrId.
    StrId val_again = pool_intern(&g_db.strings, "val", 3);
    assert(val_again.idx == g_db.names.val.idx);

    teardown();
}

static void test_lifecycle_scalar_defaults(void) {
    setup();
    assert(g_db.current_revision == 1);
    assert(g_db.request_revision == 0);
    assert(g_db.invalidation_enabled == true);
    assert(g_db.comptime_depth_limit == 256);
    teardown();
}

// db_init → db_free → db_init must produce a clean working db both
// times. Catches: state not fully zeroed on free, static pointers
// lingering, double-free on re-init, etc.
static void test_lifecycle_init_free_init_idempotent(void) {
    db_init(&g_db);
    DefId d1 = db_create_def(&g_db);
    (void)d1;
    db_free(&g_db);

    db_init(&g_db);
    DefId d2 = db_create_def(&g_db);
    // After re-init, DefIds restart from 1 (slot 0 is NONE sentinel).
    assert(d2.idx == 1);
    assert(g_db.names.sizeOf.idx != 0);
    assert(g_db.current_revision == 1);
    db_free(&g_db);

    memset(&g_db, 0, sizeof(g_db));
}

/* ---------------------------------------------------------------------- */
/* collect + ast_dep tests                                                */
/* ---------------------------------------------------------------------- */

// Visitor that counts calls and tallies per-kind hits.
struct collect_count {
    size_t total;
    size_t by_kind[QUERY_KIND_COUNT];
};

static void counting_visitor(QuerySlot *slot, QueryKind kind,
                             const void *key, void *user_data) {
    (void)slot;
    (void)key;
    struct collect_count *c = (struct collect_count *)user_data;
    c->total++;
    if ((int)kind >= 0 && (int)kind < QUERY_KIND_COUNT) {
        c->by_kind[kind]++;
    }
}

// Allocating N defs + M scopes should produce exactly N * 4 def-column
// slot visits and M * 1 scope-column slot visits.
static void test_collect_walks_def_and_scope_slots(void) {
    setup();
    const int N_DEFS = 3;
    const int M_SCOPES = 2;
    for (int i = 0; i < N_DEFS; i++)  (void)db_create_def(&g_db);
    for (int i = 0; i < M_SCOPES; i++) (void)db_create_scope(&g_db);

    struct collect_count c = {0};
    db_for_each_slot(&g_db, counting_visitor, &c);

    assert(c.by_kind[QUERY_TYPE_OF_DECL] == N_DEFS);
    assert(c.by_kind[QUERY_FN_SIGNATURE] == N_DEFS);
    assert(c.by_kind[QUERY_IS_COMPTIME]  == N_DEFS);
    assert(c.by_kind[QUERY_CONST_EVAL]   == N_DEFS);
    assert(c.by_kind[QUERY_RESOLVE_REF]  == M_SCOPES);
    // No modules allocated — no module-slot visits.
    assert(c.by_kind[QUERY_MODULE_AST] == 0);
    teardown();
}

// Allocating a module should give us four module-slot visits (one per
// kind). The keys passed to the visitor must round-trip through
// db_locate_slot to the same slot pointer.
static void test_collect_walks_module_slots(void) {
    setup();
    ModuleId mid = db_create_module(&g_db, STR_ID_NONE);
    assert(module_id_valid(mid));

    struct collect_count c = {0};
    db_for_each_slot(&g_db, counting_visitor, &c);

    assert(c.by_kind[QUERY_MODULE_AST]       == 1);
    assert(c.by_kind[QUERY_TOP_LEVEL_INDEX]  == 1);
    assert(c.by_kind[QUERY_MODULE_EXPORTS]   == 1);
    assert(c.by_kind[QUERY_MODULE_DEF_MAP]   == 1);

    // Round-trip: db_locate_slot with this ModuleId returns the same
    // slot that collect walked.
    QuerySlot *located = db_locate_slot(&g_db, QUERY_MODULE_AST, &mid);
    struct ModuleInfo *mod = db_get_module(&g_db, mid);
    assert(located == &mod->slot_module_ast);
    teardown();
}

// record_ast_dep_for_def: inside a parent query body, call the helper
// for a def whose owning module's AST slot is DONE. The parent's frame
// should pick up a dep entry on QUERY_MODULE_AST.
static void test_ast_dep_records_dep_for_def(void) {
    setup();

    // Allocate a module and stamp its parent_modules entry for a def.
    ModuleId mid = db_create_module(&g_db, STR_ID_NONE);
    DefId d = db_create_def(&g_db);
    ((ModuleId *)vec_get(&g_db.defs.parent_modules, d.idx))[0] = mid;

    // Stamp the module's AST slot as DONE with a fingerprint (LSP input
    // setup). Use db_locate_slot to find the real slot location.
    QuerySlot *ast_slot = db_locate_slot(&g_db, QUERY_MODULE_AST, &mid);
    assert(ast_slot != NULL);
    ast_slot->state = QUERY_DONE;
    ast_slot->fingerprint = 0xA51CAFE;
    ast_slot->computed_rev = g_db.current_revision;
    ast_slot->verified_rev = g_db.current_revision;

    // Open a parent query (compute on def d's type slot).
    QueryBeginResult r = db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &d);
    assert(r == QUERY_BEGIN_COMPUTE);

    // Inside the parent body: record an AST dep on the def's module.
    db_record_ast_dep_for_def(&g_db, d);

    // The parent's frame should now hold one dep — on the module's AST.
    QueryFrame *top = db_query_stack_top(&g_db);
    assert(top != NULL);
    assert(top->deps != NULL);
    assert(top->deps->count == 1);
    QueryDep *dep = (QueryDep *)vec_get(top->deps, 0);
    assert(dep->kind == QUERY_MODULE_AST);
    assert(dep->dep_fp == 0xA51CAFE);

    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &d, 42);
    teardown();
}

/* ---------------------------------------------------------------------- */
/* Workspace bootstrap tests: sources, module init/reset/free, AstIdMap.  */
/* ---------------------------------------------------------------------- */

static void test_alloc_source_round_trip(void) {
    setup();
    const char *path = "src/foo.ore";
    const char *text = "fn main() {}";
    SourceId sid = db_create_source(&g_db, path, strlen(path), text, strlen(text));

    assert(sid.idx == 1);  // idx 0 is the NONE sentinel.
    struct Source *src = (struct Source *)vec_get(&g_db.sources, sid.idx);
    assert(src != NULL);
    assert(src->text_len == (uint32_t)strlen(text));
    assert(memcmp(src->text, text, strlen(text)) == 0);
    assert(src->text[strlen(text)] == '\0');  // NUL terminator added.
    assert(src->hash != 0);
    assert(src->version == 1);
    assert(!file_id_is_virtual(src->file_id));
    teardown();
}

static void test_alloc_source_interns_path(void) {
    setup();
    SourceId a = db_create_source(&g_db, "x.ore", 5, "", 0);
    SourceId b = db_create_source(&g_db, "x.ore", 5, "", 0);
    struct Source *sa = (struct Source *)vec_get(&g_db.sources, a.idx);
    struct Source *sb = (struct Source *)vec_get(&g_db.sources, b.idx);
    // Same path → same interned StrId, but distinct SourceIds.
    assert(sa->path.idx == sb->path.idx);
    assert(a.idx != b.idx);
    teardown();
}

static void test_module_info_init_sets_identity_and_slots(void) {
    setup();
    StrId name = pool_intern(&g_db.strings, "mymod", 5);
    FileId fid = file_id_make_physical(7);

    ModuleId mid = db_create_module(&g_db, STR_ID_NONE);
    struct ModuleInfo *mod = db_get_module(&g_db, mid);
    module_info_init(mod, mid, name, fid);

    assert(mod->id.idx == mid.idx);
    assert(mod->name.idx == name.idx);
    assert(file_id_eq(mod->file, fid));

    // Slots initialized with correct kinds (not zero).
    assert(mod->slot_module_ast.kind        == QUERY_MODULE_AST);
    assert(mod->slot_top_level_index.kind   == QUERY_TOP_LEVEL_INDEX);
    assert(mod->slot_module_exports.kind    == QUERY_MODULE_EXPORTS);
    assert(mod->slot_module_def_map.kind    == QUERY_MODULE_DEF_MAP);
    assert(mod->slot_module_ast.state       == QUERY_EMPTY);

    // Per-module arena is live (can alloc).
    void *probe = arena_alloc(&mod->arena, 64);
    assert(probe != NULL);
    teardown();
}

static void test_module_info_reset_wipes_arena_keeps_slots(void) {
    setup();
    ModuleId mid = db_create_module(&g_db, STR_ID_NONE);
    struct ModuleInfo *mod = db_get_module(&g_db, mid);
    module_info_init(mod, mid, STR_ID_NONE, FILE_ID_NONE);

    // Simulate parser-pass allocations into mod->arena.
    void *p1 = arena_alloc(&mod->arena, 4096);
    assert(p1 != NULL);
    mod->ast = (struct ASTStore *)0xDEADBEEF; // pretend pointer into mod->arena
    mod->ast_id_map = (struct AstIdMap *)0xCAFE; // same

    // Stamp the AST slot as DONE so we can verify reset doesn't touch it.
    mod->slot_module_ast.state = QUERY_DONE;
    mod->slot_module_ast.fingerprint = 0x1234;

    module_info_reset(mod);

    // Per-module pointers wiped.
    assert(mod->ast == NULL);
    assert(mod->ast_id_map == NULL);

    // Vec structs zeroed (count=0, capacity=0).
    assert(mod->span_map.count == 0);
    assert(mod->span_map.capacity == 0);
    assert(mod->top_level_index.count == 0);

    // Slots survive — the query engine owns their lifecycle.
    assert(mod->slot_module_ast.state == QUERY_DONE);
    assert(mod->slot_module_ast.fingerprint == 0x1234);

    // Arena is fresh again — can allocate from offset 0.
    void *p2 = arena_alloc(&mod->arena, 128);
    assert(p2 != NULL);
    teardown();
}

static void test_ast_id_map_insert_and_get(void) {
    setup();
    ModuleId mid = db_create_module(&g_db, STR_ID_NONE);
    struct ModuleInfo *mod = db_get_module(&g_db, mid);
    module_info_init(mod, mid, STR_ID_NONE, FILE_ID_NONE);

    // Lazy-alloc the AstIdMap in the module's arena (mirrors what the
    // parser's top-level-index pass will do).
    mod->ast_id_map = (struct AstIdMap *)arena_alloc(
        &mod->arena, sizeof(struct AstIdMap));
    ast_id_map_init(mod->ast_id_map, &mod->arena);

    StrId name_a = pool_intern(&g_db.strings, "foo", 3);
    StrId name_b = pool_intern(&g_db.strings, "bar", 3);

    AstId id_a = ast_id_map_insert(mod->ast_id_map, KIND_FUNCTION,
                                   name_a, (AstNodeId){.idx = 10});
    AstId id_b = ast_id_map_insert(mod->ast_id_map, KIND_STRUCT,
                                   name_b, (AstNodeId){.idx = 20});

    assert(ast_id_valid(id_a));
    assert(ast_id_valid(id_b));
    assert(id_a.idx != id_b.idx);

    AstNodeId got_a = ast_id_map_get(mod->ast_id_map, id_a);
    AstNodeId got_b = ast_id_map_get(mod->ast_id_map, id_b);
    assert(ast_node_id_valid(got_a) && got_a.idx == 10);
    assert(ast_node_id_valid(got_b) && got_b.idx == 20);

    // Lookup with NONE returns NONE.
    AstNodeId none = ast_id_map_get(mod->ast_id_map, AST_ID_NONE);
    assert(!ast_node_id_valid(none));
    teardown();
}

// Regression test for the slot-resource leak. Drive a slot through a
// compute that records deps (allocating a malloc-backed Vec backing
// buffer). db_free must release that buffer — otherwise LSan (where
// available) reports a leak. On darwin LSan isn't enabled with ASan,
// but the slot_release visitor still runs and zeroes the field, which
// we assert below by re-init-ing and checking state.
static void test_db_free_releases_slot_deps(void) {
    setup();
    DefId b = alloc_def();
    DefId a = alloc_def();

    // Compute B so the parent's record_dep_on_parent allocates a deps
    // Vec backing buffer when A's begin sees B cached.
    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &b);
    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &b, 1);

    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &a);
    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &b);  // cached → records dep
    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &a, 2);

    // slot_a->deps now non-NULL with backing buffer.
    assert(slot_for_def(a)->deps != NULL);
    assert(slot_for_def(a)->deps->data != NULL);

    // Teardown runs the slot_release visitor inside db_free. We can't
    // observe the freed memory after teardown(), but ASan/UBSan
    // catching double-free or UAF on subsequent setup() proves the
    // release path is sound.
    teardown();

    // Re-init and confirm a fresh db can still alloc deps without
    // tripping over stale state from the prior session.
    setup();
    DefId c = alloc_def();
    db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &c);
    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &c, 3);
    assert(slot_for_def(c)->state == QUERY_DONE);
    teardown();
}

// record_ast_dep_for_span: exercises the db_get_file_module lookup path.
// Allocate a module with a known FileId, stamp its AST slot DONE, then
// record a dep using a CompactSpan referencing that file from inside a
// parent query.
static void test_ast_dep_records_dep_for_span(void) {
    setup();

    // Allocate a module and stamp its FileId directly. db_create_module
    // sets identity; we patch the file field so db_get_file_module can
    // match it. (In real usage module_info_init would set this.)
    ModuleId mid = db_create_module(&g_db, STR_ID_NONE);
    struct ModuleInfo *mod = db_get_module(&g_db, mid);
    assert(mod != NULL);
    FileId fid = file_id_make_physical(1);
    mod->file = fid;

    // Sanity: module_for_file finds it.
    assert(module_id_eq(db_get_file_module(&g_db, fid), mid));

    // Stamp the module's AST slot as DONE with a fingerprint.
    QuerySlot *ast_slot = db_locate_slot(&g_db, QUERY_MODULE_AST, &mid);
    assert(ast_slot != NULL);
    ast_slot->state = QUERY_DONE;
    ast_slot->fingerprint = 0xBEEFCAFE;
    ast_slot->computed_rev = g_db.current_revision;
    ast_slot->verified_rev = g_db.current_revision;

    // Open a parent query.
    DefId d = db_create_def(&g_db);
    QueryBeginResult r = db_query_begin(&g_db, QUERY_TYPE_OF_DECL, &d);
    assert(r == QUERY_BEGIN_COMPUTE);

    // Record an AST dep via span. Span's byte range doesn't matter for
    // this path — only span.file is consulted.
    CompactSpan span = { .file = fid, .byte_start = 0, .byte_end = 0 };
    db_record_ast_dep_for_span(&g_db, span);

    // Parent frame must hold one dep on the module's AST.
    QueryFrame *top = db_query_stack_top(&g_db);
    assert(top != NULL);
    assert(top->deps != NULL);
    assert(top->deps->count == 1);
    QueryDep *dep = (QueryDep *)vec_get(top->deps, 0);
    assert(dep->kind == QUERY_MODULE_AST);
    assert(dep->dep_fp == 0xBEEFCAFE);

    db_query_succeed(&g_db, QUERY_TYPE_OF_DECL, &d, 7);
    teardown();
}

/* ---------------------------------------------------------------------- */

int main(void) {
    test_slot_init_state();
    test_begin_compute_on_empty();
    test_succeed_marks_done_and_stores_fp();
    test_cached_on_done_at_same_rev();
    test_cycle_on_running();
    test_cancel_propagates();
    test_fail_marks_error();
    test_dep_recorded_on_parent();
    test_revalidate_recompute_on_dep_fp_mismatch();
    test_revalidate_skip_when_deps_stable();
    test_revalidate_recompute_when_untracked();
    test_error_state_revalidates();
    test_pointer_stability_under_alloc();
    test_deps_vec_reused_across_recomputes();

    test_lifecycle_pre_interned_names();
    test_lifecycle_scalar_defaults();
    test_lifecycle_init_free_init_idempotent();

    test_collect_walks_def_and_scope_slots();
    test_collect_walks_module_slots();
    test_ast_dep_records_dep_for_def();
    test_ast_dep_records_dep_for_span();

    test_alloc_source_round_trip();
    test_alloc_source_interns_path();
    test_module_info_init_sets_identity_and_slots();
    test_module_info_reset_wipes_arena_keeps_slots();
    test_ast_id_map_insert_and_get();
    test_db_free_releases_slot_deps();

    printf("query + lifecycle + collect + ast_dep + bootstrap tests: 27/27 passed\n");
    return 0;
}
