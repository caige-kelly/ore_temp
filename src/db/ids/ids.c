#include "ids.h"
#include "../db.h"
#include "../../syntax/syntax.h"  // GreenNode + green_node_release

// =============================================================================
// SoA column initialization + per-DefId / per-ScopeId allocators.
//
// Source-text, file, and module mutations are NOT here — they live in
// src/db/setters/ (source.c / file.c / module.c). Reads live in
// src/db/getters/. This file is the internal id-space + lifecycle
// plumbing only: column init/teardown plus the lockstep allocators
// for the per-def and per-scope SoA columns.
// =============================================================================

// Forward decls into setters/source.c for the texts free path used by
// db_ids_free below. Defined in src/db/setters/source.c.
void db_source_free_texts(struct db *s);

void db_ids_init(struct db *s) {
  /* ---- Sources / files / modules — X-macro driven rowed columns ------- */

  // sources SoA — fully rowed (one zero sentinel row).
#define X(name, type)                                                          \
  vec_init(&s->sources.name, sizeof(type));                                    \
  vec_push_zero(&s->sources.name);
  ORE_SOURCES_COLUMNS(X)
#undef X

  // files SoA — the per-file parse unit (QUERY_FILE_AST keyed here).
  // Fully rowed (one zero sentinel row). The 3rd X-macro arg is the
  // eviction action — irrelevant here, ignored.
#define X(name, type, _evict)                                                  \
  vec_init(&s->files.name, sizeof(type));                                      \
  vec_push_zero(&s->files.name);
  ORE_FILES_COLUMNS(X)
#undef X

  // modules SoA — plain rowed columns (one zero sentinel row).
  // No flat-pool pair: "files in module M" is a filter scan over the
  // files.module_id back-ref; nothing to init here beyond the X-macro.
#define X(name, type)                                                          \
  vec_init(&s->namespaces.name, sizeof(type));                                 \
  vec_push_zero(&s->namespaces.name);
  ORE_NAMESPACES_COLUMNS(X)
#undef X

  /* ---- defs: thin shared SoA + 8 per-kind tables + shared pools ------- */

  // Thin db.defs — identity + routing, indexed directly by DefId.
#define X(name, type)                                                          \
  vec_init(&s->defs.name, sizeof(type));                                       \
  vec_push_zero(&s->defs.name);
  ORE_DEFS_COLUMNS(X)
#undef X

  // Per-kind tables. Row 0 of each is a reserved sentinel — kind_row
  // defaults to 0, so a stray access on an unclassified def stays
  // in-bounds; real rows (from db_def_set_kind) start at 1.
#define X(name, type)                                                          \
  vec_init(&s->fns.name, sizeof(type));                                        \
  vec_push_zero(&s->fns.name);
  ORE_FNS_COLUMNS(X)
#undef X
#define X(name, type)                                                          \
  vec_init(&s->structs.name, sizeof(type));                                    \
  vec_push_zero(&s->structs.name);
  ORE_STRUCTS_COLUMNS(X)
#undef X
#define X(name, type)                                                          \
  vec_init(&s->unions.name, sizeof(type));                                     \
  vec_push_zero(&s->unions.name);
  ORE_UNIONS_COLUMNS(X)
#undef X
#define X(name, type)                                                          \
  vec_init(&s->enums.name, sizeof(type));                                      \
  vec_push_zero(&s->enums.name);
  ORE_ENUMS_COLUMNS(X)
#undef X
#define X(name, type)                                                          \
  vec_init(&s->effects.name, sizeof(type));                                    \
  vec_push_zero(&s->effects.name);
  ORE_EFFECTS_COLUMNS(X)
#undef X
#define X(name, type)                                                          \
  vec_init(&s->handlers.name, sizeof(type));                                   \
  vec_push_zero(&s->handlers.name);
  ORE_HANDLERS_COLUMNS(X)
#undef X
#define X(name, type)                                                          \
  vec_init(&s->variables.name, sizeof(type));                                  \
  vec_push_zero(&s->variables.name);
  ORE_VARIABLES_COLUMNS(X)
#undef X
#define X(name, type)                                                          \
  vec_init(&s->constants.name, sizeof(type));                                  \
  vec_push_zero(&s->constants.name);
  ORE_CONSTANTS_COLUMNS(X)
#undef X

  // resolve_ref / resolve_path — dense slot tables for the HashMap-keyed
  // queries. Row 0 is a reserved sentinel; the routing HashMaps map
  // real keys to rows >= 1.
#define X(tbl)                                                                 \
  vec_init(&s->tbl.results, sizeof(DefId));                                    \
  vec_init(&s->tbl.slots_hot, sizeof(struct QuerySlotHot));                    \
  vec_init(&s->tbl.slots_cold, sizeof(struct QuerySlotCold));                  \
  vec_push_zero(&s->tbl.results);                                              \
  vec_push_zero(&s->tbl.slots_hot);                                            \
  vec_push_zero(&s->tbl.slots_cold);
  X(resolve_ref)
  X(resolve_path)
#undef X

  // def_identity — adds a `keys` column (parallel SyntaxNodePtr) so the
  // dispatch thunk can recover the original call arg from a routing-
  // key collision; same row layout otherwise.
  vec_init(&s->def_identity.results, sizeof(DefId));
  vec_init(&s->def_identity.keys, sizeof(SyntaxNodePtr));
  vec_init(&s->def_identity.slots_hot, sizeof(struct QuerySlotHot));
  vec_init(&s->def_identity.slots_cold, sizeof(struct QuerySlotCold));
  vec_push_zero(&s->def_identity.results);
  vec_push_zero(&s->def_identity.keys);
  vec_push_zero(&s->def_identity.slots_hot);
  vec_push_zero(&s->def_identity.slots_cold);

  // decl_ast — results holds SyntaxNodePtr (not DefId). `keys` mirrors
  // the original call arg so recompute_decl_ast can recover it.
  vec_init(&s->decl_ast.results, sizeof(SyntaxNodePtr));
  vec_init(&s->decl_ast.keys, sizeof(SyntaxNodePtr));
  vec_init(&s->decl_ast.slots_hot, sizeof(struct QuerySlotHot));
  vec_init(&s->decl_ast.slots_cold, sizeof(struct QuerySlotCold));
  vec_push_zero(&s->decl_ast.results);
  vec_push_zero(&s->decl_ast.keys);
  vec_push_zero(&s->decl_ast.slots_hot);
  vec_push_zero(&s->decl_ast.slots_cold);

  // Centralized diagnostics — dense Vec<DiagList>, row 0 a reserved
  // sentinel (the routing HashMap maps real units to rows >= 1).
  vec_init(&s->diag_lists, sizeof(DiagList));
  vec_push_zero(&s->diag_lists);

  // Body-scope pools — pure append arrays, range-addressed.
  vec_init(&s->body_scope_rows, sizeof(ScopeRow));
  vec_init(&s->body_scope_binds, sizeof(ScopedBind));

  // Per-decl resolved-types: each per-decl query builds a HashMap-backed
  // NodeTypesRange and persists it on its per-kind column. There is no
  // shared pool — every range owns its own HashMap. The empty sentinel
  // is just a zero NodeTypesRange (uninitialized HashMap), which
  // sema_node_types_range_lookup short-circuits via hashmap_is_initialized.
  s->empty_node_types_range = (NodeTypesRange){0};

  /* ---- scopes SoA ------------------------------------------------------ */

  // Plain rowed columns (one zero sentinel row). decl_lo/decl_len join
  // the X-macro so they're initialized in lockstep with parents/meta/...
  // — scope 0 is a sentinel with empty range (lo=0, len=0).
#define X(name, type)                                                          \
  vec_init(&s->scopes.name, sizeof(type));                                     \
  vec_push_zero(&s->scopes.name);
  ORE_SCOPES_COLUMNS(X)
#undef X
  vec_init(&s->scopes.decl_pool, sizeof(DeclEntry));

  /* ---- query stack ----------------------------------------------------- */

  vec_init_in_arena(&s->query_stack, &s->arena, 256, sizeof(struct QueryFrame));

  // running_slots is request-scoped scratch — pushed by db_query_begin
  // on every COMPUTE transition, swept by db_request_end. Malloc-backed
  // (NOT request_arena) so the backing buffer persists across requests
  // and amortizes growth.
  vec_init(&s->running_slots, sizeof(QueryRunningRef));
}

// Reserve a fresh DefId. Every defs column grows by one zero row in
// lockstep so DefId(N) names row N everywhere. Called from query
// bodies that materialize new defs (def_identity); NOT exposed as an
// external setter.
DefId db_create_def(struct db *s) {
  uint32_t idx = (uint32_t)s->defs.names.count;

#define X(name, type) vec_push_zero(&s->defs.name);
  ORE_DEFS_COLUMNS(X)
#undef X
  // kinds[idx] = KIND_NONE and kind_row[idx] = 0 (both zeroed). The def
  // is classified later by db_def_set_kind, which allocates its row in
  // the matching per-kind table.
  return (DefId){.idx = idx};
}

// Classify a def: record its DefKind and allocate its row in the
// matching per-kind table. Idempotent for the same kind; a def's kind
// is fixed for the db's lifetime (ast_id_compute folds the decl's AST
// kind into the AstId, so a kind change would yield a different DefId).
void db_def_set_kind(struct db *s, DefId def, DefKind kind) {
  DefKind *kslot = (DefKind *)vec_get(&s->defs.kinds, def.idx);
  if (*kslot == kind)
    return;
  assert(*kslot == KIND_NONE &&
         "db_def_set_kind: a def's kind is fixed once classified");

  uint32_t row = 0;
  switch (kind) {
  case KIND_FUNCTION:
    row = (uint32_t)s->fns.type.count;
#define X(name, type) vec_push_zero(&s->fns.name);
    ORE_FNS_COLUMNS(X)
#undef X
    break;
  case KIND_STRUCT:
    row = (uint32_t)s->structs.type.count;
#define X(name, type) vec_push_zero(&s->structs.name);
    ORE_STRUCTS_COLUMNS(X)
#undef X
    break;
  case KIND_UNION:
    row = (uint32_t)s->unions.type.count;
#define X(name, type) vec_push_zero(&s->unions.name);
    ORE_UNIONS_COLUMNS(X)
#undef X
    break;
  case KIND_ENUM:
    row = (uint32_t)s->enums.type.count;
#define X(name, type) vec_push_zero(&s->enums.name);
    ORE_ENUMS_COLUMNS(X)
#undef X
    break;
  case KIND_EFFECT:
    row = (uint32_t)s->effects.type.count;
#define X(name, type) vec_push_zero(&s->effects.name);
    ORE_EFFECTS_COLUMNS(X)
#undef X
    break;
  case KIND_HANDLER:
    row = (uint32_t)s->handlers.type.count;
#define X(name, type) vec_push_zero(&s->handlers.name);
    ORE_HANDLERS_COLUMNS(X)
#undef X
    break;
  case KIND_VARIABLE:
    row = (uint32_t)s->variables.type.count;
#define X(name, type) vec_push_zero(&s->variables.name);
    ORE_VARIABLES_COLUMNS(X)
#undef X
    break;
  case KIND_CONSTANT:
    row = (uint32_t)s->constants.type.count;
#define X(name, type) vec_push_zero(&s->constants.name);
    ORE_CONSTANTS_COLUMNS(X)
#undef X
    break;
  case KIND_NONE:
  default:
    assert(0 && "db_def_set_kind: cannot set KIND_NONE");
    return;
  }

  *kslot = kind;
  *(uint32_t *)vec_get(&s->defs.kind_row, def.idx) = row;
}

// Reserve a fresh ScopeId. Called from query bodies (module_exports
// allocates internal + export scopes; body_scopes allocates per-fn).
// decl_lo/decl_len are zero-init'd via the X-macro; callers stamp the
// scope's range after appending its decls to decl_pool.
ScopeId db_create_scope(struct db *s) {
  uint32_t idx = (uint32_t)s->scopes.parents.count;

#define X(name, type) vec_push_zero(&s->scopes.name);
  ORE_SCOPES_COLUMNS(X)
#undef X

  return (ScopeId){.idx = idx};
}

// AstId — content-addressed identity for AST nodes (kind, name).
// Stable across reparses when the (kind, name) pair is preserved.
AstId ast_id_compute(uint32_t kind, StrId name) {
  uint64_t h = 0xcbf29ce484222325ULL;
  h ^= (uint64_t)kind;
  h *= 0x100000001b3ULL;
  h ^= (uint64_t)name.idx;
  h *= 0x100000001b3ULL;
  return (AstId){.idx = (uint32_t)(h ^ (h >> 32))};
}

// Free all malloc-backed Vec storage on the database. Called from db_free.
void db_ids_free(struct db *s) {
  // Source-text buffers (malloc-owned, see db_set_source_text).
  db_source_free_texts(s);

#define X(name, type) vec_free(&s->sources.name);
  ORE_SOURCES_COLUMNS(X)
#undef X

  for (size_t i = 0; i < s->files.ids.count; i++) {
    // Green tree root holds a +1 refcount per file; drop it (NodeCache
    // still has its own +1, released at db_free via node_cache_destroy).
    struct GreenNode **gslot =
        (struct GreenNode **)vec_get(&s->files.green_roots, i);
    if (*gslot) {
      green_node_release(*gslot);
      *gslot = NULL;
    }
    // Free the per-file sparse node→def map's bucket storage.
    hashmap_free((HashMap *)vec_get(&s->files.node_to_def, i));
    // top_level_indices / line_starts / tokens / imports are FileArrays
    // whose data lives in this file's arena — reclaimed by arena_free.
    arena_free((Arena *)vec_get(&s->files.arenas, i));
  }
#define X(name, type, _evict) vec_free(&s->files.name);
  ORE_FILES_COLUMNS(X)
#undef X

#define X(name, type) vec_free(&s->namespaces.name);
  ORE_NAMESPACES_COLUMNS(X)
#undef X

#define X(name, type) vec_free(&s->defs.name);
  ORE_DEFS_COLUMNS(X)
#undef X
  // Per-kind tables. Each slot column's per-slot deps buffers were
  // already released by slot_release_visitor (db_for_each_slot, run from
  // db_free before db_ids_free); here we free the column buffers.
  //
  // db.fns owns a per-row HashMap column (scope_map) and per-row
  // NodeTypesRange columns (body_node_types, signature_node_types) whose
  // HashMap buckets are heap-backed. Free each non-empty map before
  // dropping the column vec.
  for (uint32_t i = 0; i < s->fns.scope_map.count; i++) {
    HashMap *m = (HashMap *)vec_get(&s->fns.scope_map, i);
    if (hashmap_is_initialized(m))
      hashmap_free(m);
  }
  for (uint32_t i = 0; i < s->fns.body_node_types.count; i++) {
    NodeTypesRange *r =
        (NodeTypesRange *)vec_get(&s->fns.body_node_types, i);
    if (hashmap_is_initialized(&r->types))
      hashmap_free(&r->types);
  }
  for (uint32_t i = 0; i < s->fns.signature_node_types.count; i++) {
    NodeTypesRange *r =
        (NodeTypesRange *)vec_get(&s->fns.signature_node_types, i);
    if (hashmap_is_initialized(&r->types))
      hashmap_free(&r->types);
  }
#define X(name, type) vec_free(&s->fns.name);
  ORE_FNS_COLUMNS(X)
#undef X
#define X(name, type) vec_free(&s->structs.name);
  ORE_STRUCTS_COLUMNS(X)
#undef X
#define X(name, type) vec_free(&s->unions.name);
  ORE_UNIONS_COLUMNS(X)
#undef X
#define X(name, type) vec_free(&s->enums.name);
  ORE_ENUMS_COLUMNS(X)
#undef X
#define X(name, type) vec_free(&s->effects.name);
  ORE_EFFECTS_COLUMNS(X)
#undef X
#define X(name, type) vec_free(&s->handlers.name);
  ORE_HANDLERS_COLUMNS(X)
#undef X
#define X(name, type) vec_free(&s->variables.name);
  ORE_VARIABLES_COLUMNS(X)
#undef X
#define X(name, type) vec_free(&s->constants.name);
  ORE_CONSTANTS_COLUMNS(X)
#undef X
#define X(tbl)                                                                 \
  vec_free(&s->tbl.results);                                                   \
  vec_free(&s->tbl.slots_hot);                                                 \
  vec_free(&s->tbl.slots_cold);
  X(resolve_ref)
  X(resolve_path)
#undef X
  // def_identity + decl_ast also own a `keys` column.
  vec_free(&s->def_identity.results);
  vec_free(&s->def_identity.keys);
  vec_free(&s->def_identity.slots_hot);
  vec_free(&s->def_identity.slots_cold);
  vec_free(&s->decl_ast.results);
  vec_free(&s->decl_ast.keys);
  vec_free(&s->decl_ast.slots_hot);
  vec_free(&s->decl_ast.slots_cold);
  // diag_lists — each DiagList's items Vec + arena were freed by db_free
  // before db_ids_free ran; here we drop the column buffer.
  vec_free(&s->diag_lists);
  vec_free(&s->body_scope_rows);
  vec_free(&s->body_scope_binds);

#define X(name, type) vec_free(&s->scopes.name);
  ORE_SCOPES_COLUMNS(X)
#undef X
  vec_free(&s->scopes.decl_pool);

  vec_free(&s->query_stack);
  vec_free(&s->running_slots);
}
