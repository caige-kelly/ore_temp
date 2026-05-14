#include "../db.h"

#include "../storage/arena.h"
#include "../storage/hashmap.h"
#include "../storage/stringpool.h"
#include "../ids/ids.h"
#include "../intern_pool/intern_pool.h"

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

    // 4. HashMap caches — all arena-backed against s->arena.
    hashmap_init_in(&s->module_by_path,      &s->arena);
    hashmap_init_in(&s->resolve_path,        &s->arena);
    hashmap_init_in(&s->comptime_call_cache, &s->arena);

    // 5. Pre-intern hot builtin names. Lengths are literals — pool_intern
    //    is length-typed, no strlen path.
    s->names.sizeOf   = pool_intern(&s->strings, "sizeOf",   6);
    s->names.alignOf  = pool_intern(&s->strings, "alignOf",  7);
    s->names.TypeOf   = pool_intern(&s->strings, "TypeOf",   6);
    s->names.intCast  = pool_intern(&s->strings, "intCast",  7);
    s->names.typeName = pool_intern(&s->strings, "typeName", 8);

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

void db_free(struct db *s) {
    if (!s) return;

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
