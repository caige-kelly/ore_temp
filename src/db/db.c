#include "db.h"
#include <string.h>

#include "ids/ids.h"
#include "intern_pool/intern_pool.h"
#include "query/collect.h"
#include "storage/arena.h"
#include "storage/hashmap.h"
#include "storage/stringpool.h"

// Initial-capacity defaults. Compiler-scale data; sized to amortize
// arena growth across typical workloads without overcommitting on
// idle dbs (LSP startup, one-shot CLI invocations).
#define ORE_DB_ARENA_DEFAULT_CAP (64 * 1024)
#define ORE_DB_REQUEST_ARENA_DEFAULT_CAP (64 * 1024)
#define ORE_DB_STRINGS_INITIAL_SLOTS 4096

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
  hashmap_init(&s->def_by_identity);
  hashmap_init(&s->comptime_call_cache);

// 5. Pre-intern hot names. Each X-expansion interns the string and
//    stores the resulting StrId on s->names.{id}, so the parser
//    can recognize contextual keywords by equality compare:
//      tok.string_id.idx == s->names.VAL.idx
//    No pool-padding gymnastics — StrIds are whatever the pool
//    assigns, but the lookup goes through the named field.
#define X(id, name) s->names.id = pool_intern(&s->strings, name, strlen(name));
  BUILTIN_LIST(X)
  CONTEXT_LIST(X)
#undef X

  // 6. SoA columns + arena-backed query_stack.
  db_ids_init(s);

  // 7. Scalar defaults.
  //    rev_control packs: [invalidation bit | current_rev | request_rev]
  //    Start with invalidation enabled (Salsa early cutoff is the point of
  //    the query system; disabling is the debug escape hatch). current_rev=1
  //    because slot.computed_rev defaults to 0; a successful query's first
  //    succeed must write a revision strictly greater for revalidation's
  //    freshness check. request_rev=0 (unpinned).
  s->rev_control = REV_INVALIDATION_MASK | (1ULL << 32);

  // Per-durability "last changed" = the starting revision (1), so the
  // first revalidation across any tier is exact (dur_last_changed ==
  // verified_rev → fast-path declines, exact dep walk runs).
  for (int i = 0; i < DUR_COUNT; i++)
    s->dur_last_changed[i] = 1;

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
  if (!slot)
    return;
  if (slot->deps) {
    vec_free(slot->deps);
    slot->deps = NULL;
  }
  // slot->diags' Vec OBJECT lives in diag_arena, but its backing buffer
  // is malloc-owned (vec_init/vec_push in ensure_diag_storage) — it must
  // be vec_free'd explicitly, BEFORE arena_free reclaims the Vec struct.
  // (The old "lived inside diag_arena" comment was wrong and leaked it.)
  if (slot->diags) {
    vec_free(slot->diags);
    slot->diags = NULL;
  }
  if (slot->diag_arena) {
    arena_free(slot->diag_arena);
    slot->diag_arena = NULL;
  }
  slot->diag_error_count = 0;
}

void db_free(struct db *s) {
  if (!s)
    return;

  // 1. Release per-slot heap allocations (deps backing buffers,
  //    diag_arena chunks). These are malloc-owned independent of
  //    db.arena, so arena_free won't reclaim them. Visit every slot
  //    home (SoA columns, resolve_path HashMap entries, per-module
  //    embedded slots) via db_for_each_slot.
  db_for_each_slot(s, slot_release_visitor, NULL);

  // 2. Teardown — SoA columns, HashMaps, intern pool, string pool,
  //    arenas. Module storage is fully SoA; there are no per-module
  //    sub-arenas to free.
  db_ids_free(s);

  hashmap_free(&s->comptime_call_cache);
  hashmap_free(&s->def_by_identity);
  hashmap_free(&s->resolve_path);
  hashmap_free(&s->module_by_path);

  ip_free(&s->intern);
  pool_free(&s->strings);

  arena_free(&s->request_arena);
  arena_free(&s->arena);

  *s = (struct db){0};
}
