#include "db.h"

#include "storage/arena.h"
#include "storage/hashmap.h"
#include "storage/stringpool.h"
#include "ids/ids.h"
#include "intern_pool/intern_pool.h"
#include "query/collect.h"
#include "workspace/module_info.h"

// Initial-capacity defaults. Compiler-scale data; sized to amortize
// arena growth across typical workloads without overcommitting on
// idle dbs (LSP startup, one-shot CLI invocations).
#define ORE_DB_ARENA_DEFAULT_CAP         (64 * 1024)
#define ORE_DB_REQUEST_ARENA_DEFAULT_CAP (64 * 1024)
#define ORE_DB_STRINGS_INITIAL_SLOTS     1024

// Comptime evaluator recursion depth. Bounds runaway generics and
// transitively-instantiated comptime calls. 256 is roomy for real
// programs and fast to trip on pathological input. Tunable.
#define ORE_DB_COMPTIME_DEPTH_LIMIT_DEFAULT 256

/*
    db_init / db_free.

    Single instantiation point for the compiler database. Everything
    long-lived hangs off struct db: arenas, intern pool, string pool,
    SoA columns, HashMap caches, query state.

    Init order: arenas first (db_ids_init's arena-backed query_stack
    depends on s->arena being live), then string pool and intern pool
    (independent), then HashMaps (arena-backed), then pre-intern hot
    builtin names, then db_ids_init for the SoA columns. Scalar
    defaults last.

    Free order: SoA columns first (malloc-backed Vecs that don't live
    in any arena), then HashMaps for symmetry (largely a no-op for
    arena-backed maps), then intern pool, then string pool, then
    arenas — everything else's memory lives in the arenas.
*/

void db_init(struct db *s) {
    *s = (struct db){0};

    // 1. Arenas — required before db_ids_init.
    arena_init(&s->arena, ORE_DB_ARENA_DEFAULT_CAP);
    arena_init(&s->request_arena, ORE_DB_REQUEST_ARENA_DEFAULT_CAP);

    // 2. String pool — required before name pre-interning.
    pool_init(&s->strings, ORE_DB_STRINGS_INITIAL_SLOTS);

    // 3. Intern pool — owns its own extra_arena, no order dependency.
    ip_init(&s->intern);

    // 4. HashMap caches.
    //    module_by_path grows only when files are added to the workspace
    //    — bounded, rare. Arena-backed is fine; dead buckets on the rare
    //    rehash are negligible.
    //
    //    resolve_path and comptime_call_cache grow unboundedly across an
    //    LSP session (every unique dotted-path / comptime call adds an
    //    entry). Arena-backing them would orphan dead bucket arrays into
    //    db.arena on every rehash — a slow, week-long memory leak. Use
    //    malloc backing so rehashes free the old buffer; hashmap_free in
    //    db_free reclaims the live buffer.
    hashmap_init_in(&s->module_by_path, &s->arena);
    hashmap_init(&s->resolve_path);
    hashmap_init(&s->comptime_call_cache);

    // 5. Pre-intern hot names. Lengths are literals — pool_intern is
    //    length-typed, no strlen path.
    //
    //    Builtin dispatch — sizeOf/alignOf/TypeOf/intCast/typeName.
    s->names.sizeOf   = pool_intern(&s->strings, "sizeOf",   6);
    s->names.alignOf  = pool_intern(&s->strings, "alignOf",  7);
    s->names.TypeOf   = pool_intern(&s->strings, "TypeOf",   6);
    s->names.intCast  = pool_intern(&s->strings, "intCast",  7);
    s->names.typeName = pool_intern(&s->strings, "typeName", 8);

    //    Contextual keywords — recognized at parse time by StrId compare
    //    rather than as TokenKinds. See src/lexer/token.h.
    s->names.val      = pool_intern(&s->strings, "val",      3);
    s->names.final    = pool_intern(&s->strings, "final",    5);
    s->names.raw      = pool_intern(&s->strings, "raw",      3);
    s->names.ctl      = pool_intern(&s->strings, "ctl",      3);
    s->names.override = pool_intern(&s->strings, "override", 8);
    s->names.named    = pool_intern(&s->strings, "named",    5);
    s->names.in       = pool_intern(&s->strings, "in",       2);
    s->names.scoped   = pool_intern(&s->strings, "scoped",   6);
    s->names.linear   = pool_intern(&s->strings, "linear",   6);

    // 6. SoA columns + arena-backed query_stack.
    db_ids_init(s);

    // 7. Scalar defaults.
    //    current_revision starts at 1 because slot fields default to 0
    //    ("never computed"); a successful query's first succeed call
    //    must write a revision strictly greater than the slot's init
    //    value for the revalidation walker's freshness check.
    s->current_revision     = 1;
    s->request_revision     = 0;
    s->invalidation_enabled = true;
    s->comptime_depth_limit = ORE_DB_COMPTIME_DEPTH_LIMIT_DEFAULT;
}

// Visitor for db_for_each_slot, invoked from db_free. Releases the
// heap-owned resources hanging off each slot — its deps Vec backing
// buffer (malloc-owned by vec_init/vec_push) and its diag_arena
// chunks (malloc-owned by arena_alloc_raw inside the Arena impl).
// The slot struct itself, its deps Vec object (in db.arena), and its
// diag_arena struct (in db.arena) are reclaimed when db.arena is
// freed shortly after.
static void slot_release_visitor(QuerySlot *slot, QueryKind kind,
                                 const void *key, void *user_data) {
    (void)kind;
    (void)key;
    (void)user_data;
    if (!slot) return;
    if (slot->deps) {
        vec_free(slot->deps);
        slot->deps = NULL;
    }
    if (slot->diag_arena) {
        arena_free(slot->diag_arena);
        slot->diag_arena = NULL;
    }
    // slot->diags lived inside diag_arena, now gone.
    slot->diags = NULL;
    slot->diag_error_count = 0;
}

void db_free(struct db *s) {
    if (!s) return;

    // 1. Release per-slot heap allocations (deps backing buffers,
    //    diag_arena chunks). These are malloc-owned independent of
    //    db.arena, so arena_free won't reclaim them. Visit every slot
    //    home (SoA columns, resolve_path HashMap entries, per-module
    //    embedded slots) via db_for_each_slot.
    db_for_each_slot(s, slot_release_visitor, NULL);

    // 2. Free per-module arenas. Each ModuleInfo owns malloc-backed
    //    chunks via its embedded Arena that aren't reclaimed by
    //    db.arena (which only owns the ModuleInfo struct's bytes).
    //    Skip idx 0 — NONE sentinel.
    for (size_t i = 1; i < s->modules.count; i++) {
        struct ModuleInfo **mp =
            (struct ModuleInfo **)vec_get(&s->modules, i);
        if (mp && *mp) module_info_free(*mp);
    }

    // 3. Existing teardown — SoA columns, HashMaps, intern pool,
    //    string pool, arenas.
    db_ids_free(s);

    hashmap_free(&s->comptime_call_cache);
    hashmap_free(&s->resolve_path);
    hashmap_free(&s->module_by_path);

    ip_free(&s->intern);
    pool_free(&s->strings);

    arena_free(&s->request_arena);
    arena_free(&s->arena);

    *s = (struct db){0};
}
