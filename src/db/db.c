// db.c holds db_init / db_free only. Everything else lives under the
// input/query boundary:
//   src/db/inputs/    — mutators (db_create_*, db_set_*, db_add_*, db_emit_*)
//   src/db/getters/   — readers  (db_get_*, db_lookup_*, db_collect_*,
//                                 db_format_*, db_print_*, db_resolve_*)
//   src/db/query/     — derived queries (db_query_*)

#include "db.h"
#include <string.h>

#include "../support/data_structure/arena.h"
#include "../support/data_structure/hashmap.h"
#include "../support/data_structure/stringpool.h"
#include "../syntax/syntax.h"           // NodeCache, node_cache_new/destroy
#include "ids/ids.h"
#include "intern_pool/intern_pool.h"
#include "query/engine.h"                // db_engine_init / db_engine_free

// ----------------------------------------------------------------------------
// Primitive type defs — synthetic DefIds for u8, bool, usize, ...
//
// Allocated once at db_init into a synthetic scope (s->primitives_scope).
// Every namespace's internal scope is parented to that scope by
// module_exports, so the existing parent-walk in db_query_resolve_ref
// finds primitive names without any special lookup table.
//
// Two short-circuits avoid abusing the salsa machinery for nodes that
// have no AST backing and no real DefKind:
//   - resolve_ref special-cases the primitives scope in its hit branch
//     and returns the encoded DefId directly without calling
//     db_query_def_identity (which scans AstIdMaps that have no entry
//     for synthetic AstIds).
//   - type_of_def early-returns the matching IpIndex for any DefId in
//     the primitive range before its DB_QUERY_GUARD, so the per-kind
//     table assertions are not triggered for KIND_NONE primitives.
//
// Order of the table below IS the order of the contiguous DefId block —
// don't reorder without updating db_primitive_type_for(). The IpIndex
// constants come from src/db/intern_pool/ip_primitives.def; the StrIds
// come from PRIMITIVE_LIST in src/db/names.inc (both pre-interned at
// db_init steps 4 & 5).
struct PrimitiveSeed {
  StrId name;
  IpIndex type;
};

static void db_init_primitives(struct db *s) {
  struct PrimitiveSeed seeds[] = {
      {s->names.BOOL, IP_BOOL_TYPE},
      {s->names.ANYTYPE, IP_ANYTYPE_TYPE},
      {s->names.VOID, IP_VOID_TYPE},
      {s->names.NORETURN, IP_NORETURN_TYPE},
      {s->names.TYPE_NAME, IP_TYPE_TYPE},
      {s->names.COMPTIME_INT, IP_COMPTIME_INT_TYPE},
      {s->names.COMPTIME_FLOAT, IP_COMPTIME_FLOAT_TYPE},
      {s->names.ERROR_NAME, IP_ERROR_TYPE},
      {s->names.F32, IP_F32_TYPE},
      {s->names.F64, IP_F64_TYPE},
      {s->names.U8, IP_U8_TYPE},
      {s->names.U16, IP_U16_TYPE},
      {s->names.U32, IP_U32_TYPE},
      {s->names.U64, IP_U64_TYPE},
      {s->names.USIZE, IP_USIZE_TYPE},
      {s->names.I8, IP_I8_TYPE},
      {s->names.I16, IP_I16_TYPE},
      {s->names.I32, IP_I32_TYPE},
      {s->names.I64, IP_I64_TYPE},
      {s->names.ISIZE, IP_ISIZE_TYPE},
  };
  uint32_t n = (uint32_t)(sizeof(seeds) / sizeof(seeds[0]));

  // Allocate the synthetic scope. ScopeMeta is SCOPE_MODULE so any
  // future audit treating the parent walk uniformly sees the same shape
  // as a real namespace internal scope. owning_modules stays 0 (none) —
  // primitives don't belong to any user namespace.
  s->primitives_scope = db_create_scope(s);
  *(ScopeMeta *)vec_get(&s->scopes.meta, s->primitives_scope.idx) =
      SCOPE_MODULE;

  s->first_primitive_def = (DefId){.idx = (uint32_t)s->defs.names.count};
  s->primitive_count = n;

  uint32_t lo = (uint32_t)s->scopes.decl_pool.count;
  for (uint32_t i = 0; i < n; i++) {
    DefId d = db_create_def(s);
    // Fill identity columns directly. NOTE: do NOT call db_def_set_kind
    // — primitives never reach the per-kind tables (type_of_def early-
    // returns for them), and KIND_NONE keeps that path closed.
    *(StrId *)vec_get(&s->defs.names, d.idx) = seeds[i].name;

    // Push DeclEntry. node_ptr = {0} is a sentinel: resolve_ref's hit
    // branch detects this scope by ScopeId identity and never routes
    // through db_query_def_identity, so the node_ptr field is unused.
    DeclEntry de = {.name = seeds[i].name, .node_ptr = (SyntaxNodePtr){0}};
    vec_push(&s->scopes.decl_pool, &de);
  }
  *(uint32_t *)vec_get(&s->scopes.decl_lo, s->primitives_scope.idx) = lo;
  *(uint32_t *)vec_get(&s->scopes.decl_len, s->primitives_scope.idx) = n;
}

// Map a primitive DefId back to its IpIndex. Public so type_of_def can
// short-circuit. Returns IP_NONE if `def` is not in the primitive range.
IpIndex db_primitive_type_for(struct db *s, DefId def) {
  uint32_t lo = s->first_primitive_def.idx;
  uint32_t hi = lo + s->primitive_count;
  if (def.idx < lo || def.idx >= hi)
    return IP_NONE;
  // The table below mirrors the order in db_init_primitives. Same
  // local-index mapping; keeping it inline so a single grep on
  // "PrimitiveSeed" surfaces both halves.
  static const uint32_t ips[] = {
      IP_INDEX_BOOL_TYPE,
      IP_INDEX_ANYTYPE_TYPE,
      IP_INDEX_VOID_TYPE,
      IP_INDEX_NORETURN_TYPE,
      IP_INDEX_TYPE_TYPE,
      IP_INDEX_COMPTIME_INT_TYPE,
      IP_INDEX_COMPTIME_FLOAT_TYPE,
      IP_INDEX_ERROR_TYPE,
      IP_INDEX_F32_TYPE,
      IP_INDEX_F64_TYPE,
      IP_INDEX_U8_TYPE,
      IP_INDEX_U16_TYPE,
      IP_INDEX_U32_TYPE,
      IP_INDEX_U64_TYPE,
      IP_INDEX_USIZE_TYPE,
      IP_INDEX_I8_TYPE,
      IP_INDEX_I16_TYPE,
      IP_INDEX_I32_TYPE,
      IP_INDEX_I64_TYPE,
      IP_INDEX_ISIZE_TYPE,
  };
  return (IpIndex){.v = ips[def.idx - lo]};
}

// True iff `scope` is the synthetic primitives scope. Used by
// resolve_ref to short-circuit the hit branch.
bool db_is_primitives_scope(struct db *s, ScopeId scope) {
  return scope.idx != 0 && scope.idx == s->primitives_scope.idx;
}

// Look up the DefId of the i-th DeclEntry in the primitives scope. The
// scope is contiguous in decl_pool starting at decl_lo[primitives_scope],
// and the DefIds are contiguous starting at s->first_primitive_def, so
// the position-within-scope == position-within-defs.
DefId db_primitive_def_for_slot(struct db *s, uint32_t slot_in_pool) {
  uint32_t scope = s->primitives_scope.idx;
  uint32_t lo = *(uint32_t *)vec_get(&s->scopes.decl_lo, scope);
  uint32_t local = slot_in_pool - lo;
  return (DefId){.idx = s->first_primitive_def.idx + local};
}

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

  // 3.5. Green-tree NodeCache — hash-cons interner shared by every
  //      file's parse. Lives the workspace's lifetime; structural
  //      sharing accumulates across files and reparses.
  s->node_cache = node_cache_new();

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

  // 6.5. Engine lifecycle — stats counters, cancel token, the
  //      top_level_entry routing HashMap. Must follow db_ids_init
  //      (which initialized the top_level_entry PagedVec columns
  //      the HashMap routes into).
  db_engine_init(s);

  // 6a. Synthetic primitives scope + DefIds. Must happen AFTER db_ids_init
  // (uses db.defs / db.scopes vecs) and AFTER step 5 (uses pre-interned
  // s->names.X StrIds for the primitive identifiers). See db_init_primitives
  // for the architectural rationale.
  db_init_primitives(s);

  // 7. Scalar defaults. (Dispatch is now compile-time-resolved via the
  // const db_engine_recompute_dispatch[] table in engine_dispatch.c,
  // built from the ORE_QUERY_KINDS X-macro — no runtime register.)
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

  // Pool-compaction counters. Initialized to MIN_THRESHOLD so we don't
  // re-compact short-lived pools (compile-once CLI runs typically stay
  // under the threshold). After the first compaction these get updated
  // to the post-compaction count.
  s->last_compacted_body_scope_rows_count = 0;
  s->last_compacted_body_scope_binds_count = 0;
  s->last_compacted_decl_pool_count = 0;

  memset(&s->compact_stats, 0, sizeof(s->compact_stats));
  // s->compact_min_threshold left at 0; engine_compact.c falls back
  // to its private default (ORE_COMPACT_MIN_THRESHOLD) when the
  // field is zero. Override via the profile-workload harness when a
  // tighter compaction cadence is wanted.
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

  // 1. Release each diagnostic unit's malloc-owned buffers — must run
  //    before db_ids_free reclaims the diag_lists column itself.
  db_free_diag_lists(s);

  // 2. Engine teardown — reclaims top_level_entry_cache HashMap and
  //    runs the deep-free pass that releases per-slot deps Vecs +
  //    result-struct HashMaps (FnBody.scope_map, FnSignature.node_types,
  //    NodeTypesRange.types, …) for every DONE/ERROR slot. Must run
  //    BEFORE db_ids_free, which drops the columns those slots live in.
  db_engine_free(s);

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

  // NodeCache: must run AFTER db_ids_free (which dropped each file's
  // green_root +1). The cache's own +1 on each canonical node is the
  // last remaining ref; releasing the cache cascades the trees free.
  node_cache_destroy(s->node_cache);
  s->node_cache = NULL;

  arena_free(&s->request_arena);
  arena_free(&s->arena);

  *s = (struct db){0};
}
