// db.c holds db_init / db_free only. Everything else lives under the
// input/query boundary:
//   src/db/setters/   — mutators (db_create_*, db_set_*, db_add_*, db_emit_*)
//   src/db/getters/   — readers  (db_get_*, db_lookup_*, db_collect_*,
//                                 db_format_*, db_print_*, db_resolve_*)
//   src/db/query/     — derived queries (db_query_*)

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

// Comptime evaluator recursion depth. Bounds runaway comptime
// instantiation (Ore's polymorphism mechanism — `fn(comptime T: type)`
// expansions, struct-returning comptime fns, etc.). 256 is roomy for
// real programs and fast to trip on pathological input. Tunable.
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
  //    resolve_path_cache / source_by_path / comptime_call_cache grow
  //    unboundedly across an LSP session (every unique dotted-path /
  //    opened file / comptime call adds an entry). Arena-back would
  //    orphan dead bucket arrays on each rehash — a slow, week-long
  //    memory leak. Use malloc-backing so rehashes free the old
  //    buffer; hashmap_free in db_free reclaims the live buffer.
  hashmap_init(&s->source_by_path);
  hashmap_init(&s->file_by_source);
  hashmap_init(&s->resolve_path_cache);
  hashmap_init(&s->decl_ast_cache);
  hashmap_init(&s->def_by_identity);
  hashmap_init(&s->resolve_ref_cache);
  hashmap_init(&s->comptime_call_cache);
  // diags grows with the number of analysis units that emit a
  // diagnostic — malloc-backed so rehashes free the old bucket array.
  hashmap_init(&s->diags);

// 5. Pre-intern hot names. Each X-expansion interns the string and
//    stores the resulting StrId on s->names.{id}, so the parser
//    can recognize contextual keywords by equality compare:
//      tok.string_id.idx == s->names.VAL.idx
#define X(id, name) s->names.id = pool_intern(&s->strings, name, strlen(name));
  PRIMITIVE_LIST(X)
  BUILTIN_LIST(X)
  FIELD_LIST(X)
  CONTEXT_LIST(X)
#undef X

  // 6. SoA columns + arena-backed query_stack.
  db_ids_init(s);

  // 6b. Typed wrapper dispatch table — populated from dispatch.c, the
  // single bridge file that knows about both the engine's QueryKind
  // enum and every wrapper's typed signature. db_verify pulls deps
  // through this.
  db_register_query_dispatch(s);

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
// buffer (malloc-owned by vec_init/vec_push). The slot struct itself
// and its deps Vec object (in db.arena) are reclaimed when db.arena is
// freed shortly after. Diagnostics are NOT on the slot — they live in
// db.diag_lists and are freed via db_free_diag_lists.
static void slot_release_visitor(QuerySlotHot *slot, QueryKind kind,
                                 uint64_t key, void *user_data) {
  (void)kind;
  (void)key;
  (void)user_data;
  if (!slot)
    return;
  if (slot->deps) {
    vec_free(slot->deps);
    slot->deps = NULL;
  }
}

// Free each diagnostic unit's malloc-owned buffers — its items Vec
// backing buffer and arena chunks. The DiagList structs live by value in
// s->diag_lists (a Vec reclaimed by db_ids_free); this must run BEFORE
// that. Row 0 is the reserved sentinel.
static void db_free_diag_lists(struct db *s) {
  for (size_t r = 1; r < s->diag_lists.count; r++) {
    DiagList *dl = (DiagList *)vec_get(&s->diag_lists, r);
    vec_free(&dl->items);
    arena_free(&dl->arena);
  }
}

void db_free(struct db *s) {
  if (!s)
    return;

  // 1. Release per-slot heap allocations (deps backing buffers).
  //    Malloc-owned independent of db.arena, so arena_free won't reclaim.
  db_for_each_slot(s, slot_release_visitor, NULL);

  // 1b. Release each diagnostic unit's malloc-owned buffers — must run
  //     before db_ids_free reclaims the diag_lists column itself.
  db_free_diag_lists(s);

  // 2. Teardown — SoA columns, HashMaps, intern pool, string pool,
  //    arenas.
  db_ids_free(s);

  // The diags routing map (malloc-backed) — values were plain row
  // indices, so nothing per-entry to free.
  hashmap_free(&s->diags);

  hashmap_free(&s->comptime_call_cache);
  hashmap_free(&s->resolve_ref_cache);
  hashmap_free(&s->def_by_identity);
  hashmap_free(&s->resolve_path_cache);
  hashmap_free(&s->decl_ast_cache);
  hashmap_free(&s->file_by_source);
  hashmap_free(&s->source_by_path);

  ip_free(&s->intern);
  pool_free(&s->strings);

  arena_free(&s->request_arena);
  arena_free(&s->arena);

  *s = (struct db){0};
}
