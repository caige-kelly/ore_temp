// ids.c is the schema lifecycle owner — it sizes every column the
// engine threads slots through. That requires the full QuerySlotHot /
// QuerySlotCold definitions, so the ORE_ENGINE_PRIVATE guard is
// claimed here even though ids.c is not engine code by call shape.
// The slot structs are referenced only at sizeof time; their fields
// stay opaque to anything outside engine.c / engine_compact.c / etc.
#define ORE_ENGINE_PRIVATE
#include "ids.h"
#include "../../syntax/syntax.h" // GreenNode + green_node_release
#include "../db.h"
#include "../query/engine_internal.h" // QuerySlotHot, QuerySlotCold sizes

#include <stdlib.h> // free (imports body teardown)

// =============================================================================
// SoA column initialization + per-DefId / per-ScopeId allocators.
//
// Source-text, file, and module mutations are NOT here — they live in
// src/db/inputs/ (source.c / file.c / module.c). Reads live in
// src/db/getters/. This file is the internal id-space + lifecycle
// plumbing only: column init/teardown plus the lockstep allocators
// for the per-def and per-scope SoA columns.
// =============================================================================

// Forward decls into inputs/source.c for the texts free path used by
// db_ids_free below. Defined in src/db/inputs/source.c.
void db_source_free_texts(struct db *s);

void db_ids_init(struct db *s) {
  /* ---- Sources / files / modules — X-macro driven rowed columns ------- */

  // sources SoA — fully rowed (one zero sentinel row). SoA invariant:
  // row 0 reserved on EVERY column (Vec metadata AND the SOURCE_TEXT
  // slot columns); real sources start at index 1, grown in lockstep by
  // create_source_row.
#define X(name, type)                                                          \
  vec_init(&s->sources.name, sizeof(type));                                    \
  vec_push_zero(&s->sources.name);
  ORE_SOURCES_COLUMNS(X)
#undef X
#define X(name, type)                                                          \
  paged_init(&s->sources.name, sizeof(type));                                  \
  paged_push_zero(&s->sources.name);
  ORE_SOURCES_SLOT_COLUMNS(X)
#undef X

  // files SoA. Vec-backed input columns + PagedVec-backed slot columns.
  // SoA invariant: row 0 is a reserved "empty/null" sentinel on EVERY
  // column — Vec AND slot — so a FileId's local index lands at the same
  // row everywhere (file_local(fid) indexes the slot columns directly).
  // Real files start at index 1, grown in lockstep by db_create_file /
  // db_create_virtual_file.
#define X(name, type, _evict)                                                  \
  vec_init(&s->files.name, sizeof(type));                                      \
  vec_push_zero(&s->files.name);
  ORE_FILES_COLUMNS(X)
#undef X
#define X(name, type)                                                          \
  paged_init(&s->files.name, sizeof(type));                                    \
  paged_push_zero(&s->files.name);
  ORE_FILES_SLOT_COLUMNS(X)
#undef X
  // PRE-STEP UAF fix: PagedVec-backed DiagBundle columns for pointer
  // stability across realloc. Same init shape as slot columns; eviction
  // is per-row via the EVICT_FREE_DIAG_BUNDLE_PAGED macro.
#define X(name, type, _evict)                                                  \
  paged_init(&s->files.name, sizeof(type));                                    \
  paged_push_zero(&s->files.name);
  ORE_FILES_PAGED_DIAG_COLUMNS(X)
#undef X

  // namespaces SoA. Vec metadata + PagedVec slot columns. Same SoA
  // invariant as files: row 0 is the reserved sentinel on EVERY column
  // (Vec and slot); real namespaces start at index 1, grown in lockstep
  // by db_create_namespace.
#define X(name, type)                                                          \
  vec_init(&s->namespaces.name, sizeof(type));                                 \
  vec_push_zero(&s->namespaces.name);
  ORE_NAMESPACES_COLUMNS(X)
#undef X
#define X(name, type)                                                          \
  paged_init(&s->namespaces.name, sizeof(type));                               \
  paged_push_zero(&s->namespaces.name);
  ORE_NAMESPACES_PAGED_DIAG_COLUMNS(X)
  ORE_NAMESPACES_SLOT_COLUMNS(X)
#undef X

  /* ---- defs: thin shared SoA + 8 per-kind tables + shared pools ------- */

  // Thin db.defs — identity + routing, indexed directly by DefId.
#define X(name, type)                                                          \
  vec_init(&s->defs.name, sizeof(type));                                       \
  vec_push_zero(&s->defs.name);
  ORE_DEFS_COLUMNS(X)
#undef X
  // PRE-STEP: PagedVec-backed diag bundles for pointer stability.
#define X(name, type)                                                          \
  paged_init(&s->defs.name, sizeof(type));                                     \
  paged_push_zero(&s->defs.name);
  ORE_DEFS_PAGED_DIAG_COLUMNS(X)
  ORE_DEFS_PAGED_AST_ID_COLUMNS(X)
#undef X

  // Per-kind tables — PagedVec. Row 0 of each is a reserved sentinel:
  // defs.kind_row defaults to 0, so a stray access on an unclassified
  // def stays in-bounds. db_def_set_kind allocates real rows >= 1.
#define X(name, type)                                                          \
  paged_init(&s->fns.name, sizeof(type));                                      \
  paged_push_zero(&s->fns.name);
  ORE_FNS_COLUMNS(X)
#undef X
#define X(name, type)                                                          \
  paged_init(&s->structs.name, sizeof(type));                                  \
  paged_push_zero(&s->structs.name);
  ORE_STRUCTS_COLUMNS(X)
#undef X
#define X(name, type)                                                          \
  paged_init(&s->unions.name, sizeof(type));                                   \
  paged_push_zero(&s->unions.name);
  ORE_UNIONS_COLUMNS(X)
#undef X
#define X(name, type)                                                          \
  paged_init(&s->enums.name, sizeof(type));                                    \
  paged_push_zero(&s->enums.name);
  ORE_ENUMS_COLUMNS(X)
#undef X
#define X(name, type)                                                          \
  paged_init(&s->effects.name, sizeof(type));                                  \
  paged_push_zero(&s->effects.name);
  ORE_EFFECTS_COLUMNS(X)
#undef X
#define X(name, type)                                                          \
  paged_init(&s->distincts.name, sizeof(type));                                \
  paged_push_zero(&s->distincts.name);
  ORE_DISTINCTS_COLUMNS(X)
#undef X
#define X(name, type)                                                          \
  paged_init(&s->variables.name, sizeof(type));                                \
  paged_push_zero(&s->variables.name);
  ORE_VARIABLES_COLUMNS(X)
#undef X
#define X(name, type)                                                          \
  paged_init(&s->constants.name, sizeof(type));                                \
  paged_push_zero(&s->constants.name);
  ORE_CONSTANTS_COLUMNS(X)
#undef X

  // HashMap-routed dense tables — PagedVec storage so pointers stay
  // valid across sub-query growth. Row 0 is a reserved sentinel; the
  // routing HashMaps map real keys to rows >= 1.
  paged_init(&s->resolve_ref.results, sizeof(DefId));
  paged_init(&s->resolve_ref.slots_hot, sizeof(struct QuerySlotHot));
  paged_init(&s->resolve_ref.slots_cold, sizeof(struct QuerySlotCold));
  vec_init(&s->resolve_ref.free_rows, sizeof(uint32_t));
  paged_push_zero(&s->resolve_ref.results);
  paged_push_zero(&s->resolve_ref.slots_hot);
  paged_push_zero(&s->resolve_ref.slots_cold);

  // def_identity — adds a `keys` column (parallel SyntaxNodePtr) so the
  // dispatch thunk can recover the original call arg from a routing-
  // key collision; same row layout otherwise.
  paged_init(&s->def_identity.results, sizeof(DefId));
  paged_init(&s->def_identity.slots_hot, sizeof(struct QuerySlotHot));
  paged_init(&s->def_identity.slots_cold, sizeof(struct QuerySlotCold));
  vec_init(&s->def_identity.free_rows, sizeof(uint32_t));
  paged_push_zero(&s->def_identity.results);
  paged_push_zero(&s->def_identity.slots_hot);
  paged_push_zero(&s->def_identity.slots_cold);

  // top_level_entry — per-(namespace, name) slots. Same routing shape
  // as def_identity but keyed by (nsid, StrId). Engine reads/writes
  // through top_level_entry_cache (HashMap-routed); rows are append-
  // grown lazily by db_query_slot_alloc.
  paged_init(&s->top_level_entry.results, sizeof(TopLevelEntry));
  paged_init(&s->top_level_entry.keys, sizeof(StrId));
  paged_init(&s->top_level_entry.slots_hot, sizeof(struct QuerySlotHot));
  paged_init(&s->top_level_entry.slots_cold, sizeof(struct QuerySlotCold));
  vec_init(&s->top_level_entry.free_rows, sizeof(uint32_t));
  paged_push_zero(&s->top_level_entry.results);
  paged_push_zero(&s->top_level_entry.keys);
  paged_push_zero(&s->top_level_entry.slots_hot);
  paged_push_zero(&s->top_level_entry.slots_cold);

  // Body-scope pools — pure append arrays, range-addressed.
  vec_init(&s->body_scope_rows, sizeof(ScopeRow));
  vec_init(&s->body_scope_binds, sizeof(ScopedBind));
  vec_init(&s->aggregate_field_pool, sizeof(AggregateFieldEntry));
  vec_init(&s->enum_variant_pool, sizeof(EnumVariantEntry));
  vec_init(&s->namespace_field_pool, sizeof(DeclEntry));

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

  // query_stack is arena-backed (lives in s->arena, reclaimed wholesale
  // by arena_free in db_free). running_slots is engine state, init'd
  // by db_engine_init.
  vec_init_in_arena(&s->query_stack, &s->arena, 256, sizeof(struct QueryFrame));
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
  // PRE-STEP: PagedVec defs columns grow in lockstep too (diag bundles +
  // decl ast-id maps — both keyed by DefId).
#define X(name, type) paged_push_zero(&s->defs.name);
  ORE_DEFS_PAGED_DIAG_COLUMNS(X)
  ORE_DEFS_PAGED_AST_ID_COLUMNS(X)
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

  // Per-kind tables are PagedVec; the new row's index is the prior
  // count (read atomically). Each branch picks any column as the
  // count source — every column in a kind's X-macro grows in
  // lockstep so they all agree.
  uint32_t row = 0;
  switch (kind) {
  case KIND_FUNCTION:
    row = (uint32_t)paged_count(&s->fns.type);
#define X(name, type) paged_push_zero(&s->fns.name);
    ORE_FNS_COLUMNS(X)
#undef X
    break;
  case KIND_STRUCT:
    row = (uint32_t)paged_count(&s->structs.type_result);
#define X(name, type) paged_push_zero(&s->structs.name);
    ORE_STRUCTS_COLUMNS(X)
#undef X
    break;
  case KIND_UNION:
    row = (uint32_t)paged_count(&s->unions.type);
#define X(name, type) paged_push_zero(&s->unions.name);
    ORE_UNIONS_COLUMNS(X)
#undef X
    break;
  case KIND_ENUM:
    row = (uint32_t)paged_count(&s->enums.type);
#define X(name, type) paged_push_zero(&s->enums.name);
    ORE_ENUMS_COLUMNS(X)
#undef X
    break;
  case KIND_EFFECT:
    row = (uint32_t)paged_count(&s->effects.type);
#define X(name, type) paged_push_zero(&s->effects.name);
    ORE_EFFECTS_COLUMNS(X)
#undef X
    break;
  case KIND_DISTINCT:
    row = (uint32_t)paged_count(&s->distincts.type);
#define X(name, type) paged_push_zero(&s->distincts.name);
    ORE_DISTINCTS_COLUMNS(X)
#undef X
    break;
  case KIND_VARIABLE:
    row = (uint32_t)paged_count(&s->variables.type_result);
#define X(name, type) paged_push_zero(&s->variables.name);
    ORE_VARIABLES_COLUMNS(X)
#undef X
    break;
  case KIND_CONSTANT:
    row = (uint32_t)paged_count(&s->constants.type_result);
#define X(name, type) paged_push_zero(&s->constants.name);
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

// AstId — content-addressed identity for AST nodes, keyed by NAME ONLY
// (one namespace: types and values share identity; same-name-different-kind
// now collide → caught by redefinition detection, 7.1). Stable across reparses.
AstId ast_id_compute(StrId name) {
  uint64_t h = 0xcbf29ce484222325ULL;
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
#define X(name, type) paged_free(&s->sources.name);
  ORE_SOURCES_SLOT_COLUMNS(X)
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
    // line_starts is a FileArray whose body lives in this file's arena —
    // reclaimed by arena_free below.
    arena_free((Arena *)paged_get(&s->files.arenas, i));
    // imports' body is a STANDALONE malloc (not arena-backed; see
    // FileArray + QUERY_FILE_IMPORTS). The recompute/evict paths free it,
    // but the last live body is dropped here at teardown.
    free(((FileArray *)vec_get(&s->files.imports, i))->data);
    // Phase P cutover — FILE_AST's parse-diag bundle holds inner
    // Vec data + Arena chunks; release before the column-level paged_free.
    // PRE-STEP UAF fix: column promoted from Vec to PagedVec for pointer
    // stability under realloc; use paged_get / paged_count accordingly.
    diag_bundle_free((DiagBundle *)paged_get(&s->files.parse_diags, i));
  }
#define X(name, type, _evict) vec_free(&s->files.name);
  ORE_FILES_COLUMNS(X)
#undef X
#define X(name, type, _evict) paged_free(&s->files.name);
  ORE_FILES_PAGED_DIAG_COLUMNS(X)
#undef X
#define X(name, type) paged_free(&s->files.name);
  ORE_FILES_SLOT_COLUMNS(X)
#undef X

  // namespaces.items bodies are standalone mallocs (NamespaceItem[], the
  // QUERY_NAMESPACE_ITEMS result — see FileArray). Recompute frees the old
  // body; the last live one is dropped here. No per-namespace eviction
  // path, so teardown is the only other owner.
  for (size_t i = 0; i < s->namespaces.items.count; i++)
    free(((FileArray *)vec_get(&s->namespaces.items, i))->data);
  // member_files[i] is an inner Vec<FileId> (per-namespace reverse index).
  // Free each inner Vec's buffer before the outer column is freed below.
  // vec_free on the zeroed row-0 sentinel (data == NULL) is a safe no-op.
  for (size_t i = 0; i < s->namespaces.member_files.count; i++)
    vec_free((Vec *)vec_get(&s->namespaces.member_files, i));
  // Phase P cutover — namespaces.check_diags holds DiagBundle per ns.
  // PRE-STEP: PagedVec storage; use paged_count + paged_get.
  for (size_t i = 0; i < paged_count(&s->namespaces.check_diags); i++)
    diag_bundle_free((DiagBundle *)paged_get(&s->namespaces.check_diags, i));
#define X(name, type) vec_free(&s->namespaces.name);
  ORE_NAMESPACES_COLUMNS(X)
#undef X
#define X(name, type) paged_free(&s->namespaces.name);
  ORE_NAMESPACES_PAGED_DIAG_COLUMNS(X)
  ORE_NAMESPACES_SLOT_COLUMNS(X)
#undef X

  // Phase P cutover — defs.type_of_decl_diags holds DiagBundle per def.
  // PRE-STEP: PagedVec storage.
  for (size_t i = 0; i < paged_count(&s->defs.type_of_decl_diags); i++)
    diag_bundle_free((DiagBundle *)paged_get(&s->defs.type_of_decl_diags, i));
  // Phase-3.1 follow-up: decl_ast_id_maps holds standalone-malloc
  // HashMap per-row, keyed by DefId on the defs SoA. Walk the column
  // and free each inner heap BEFORE the outer paged_free below wipes
  // the slots. (Phase P P7.1.9 will fold this into reclaim_slot's
  // per-column dispatch — until then teardown is the only cleanup site.)
  for (size_t r = 0; r < paged_count(&s->defs.decl_ast_id_maps); r++)
    decl_ast_id_map_free(
        (DeclAstIdMap *)paged_get(&s->defs.decl_ast_id_maps, r));
#define X(name, type) vec_free(&s->defs.name);
  ORE_DEFS_COLUMNS(X)
#undef X
#define X(name, type) paged_free(&s->defs.name);
  ORE_DEFS_PAGED_DIAG_COLUMNS(X)
  ORE_DEFS_PAGED_AST_ID_COLUMNS(X)
#undef X
  // Per-kind tables — PagedVec. Embedded HashMaps inside DONE slots'
  // result structs (FnBody.scope_map, FnSignature.node_types.types,
  // NodeTypesRange.types in body_node_types / StructType.field_node_types
  // / VariableType.value_node_types / ConstantType.value_node_types)
  // were already released by db_engine_deep_free → reclaim_orphans on
  // the engine_free path that ran before this; nothing to walk here.
  // Phase P S2: fn_body_diags holds DiagBundle{Vec items; Arena
  // args_arena;} per fn row — both inner heaps need release before
  // the outer paged_free wipes the slots.
  for (size_t r = 0; r < paged_count(&s->fns.fn_body_diags); r++)
    diag_bundle_free((DiagBundle *)paged_get(&s->fns.fn_body_diags, r));
  // Phase P cutover — same shape for fns.signature_diags.
  for (size_t r = 0; r < paged_count(&s->fns.signature_diags); r++)
    diag_bundle_free((DiagBundle *)paged_get(&s->fns.signature_diags, r));
#define X(name, type) paged_free(&s->fns.name);
  ORE_FNS_COLUMNS(X)
#undef X
#define X(name, type) paged_free(&s->structs.name);
  ORE_STRUCTS_COLUMNS(X)
#undef X
#define X(name, type) paged_free(&s->unions.name);
  ORE_UNIONS_COLUMNS(X)
#undef X
#define X(name, type) paged_free(&s->enums.name);
  ORE_ENUMS_COLUMNS(X)
#undef X
#define X(name, type) paged_free(&s->effects.name);
  ORE_EFFECTS_COLUMNS(X)
#undef X
#define X(name, type) paged_free(&s->distincts.name);
  ORE_DISTINCTS_COLUMNS(X)
#undef X
#define X(name, type) paged_free(&s->variables.name);
  ORE_VARIABLES_COLUMNS(X)
#undef X
#define X(name, type) paged_free(&s->constants.name);
  ORE_CONSTANTS_COLUMNS(X)
#undef X
#define X(tbl)                                                                 \
  paged_free(&s->tbl.results);                                                 \
  paged_free(&s->tbl.slots_hot);                                               \
  paged_free(&s->tbl.slots_cold);                                              \
  vec_free(&s->tbl.free_rows);
  X(resolve_ref)
#undef X
  // def_identity + top_level_entry also own a `keys` column.
  paged_free(&s->def_identity.results);
  paged_free(&s->def_identity.slots_hot);
  paged_free(&s->def_identity.slots_cold);
  vec_free(&s->def_identity.free_rows);

  // top_level_entry — per-(namespace, name) slots.
  paged_free(&s->top_level_entry.results);
  paged_free(&s->top_level_entry.keys);
  paged_free(&s->top_level_entry.slots_hot);
  paged_free(&s->top_level_entry.slots_cold);
  vec_free(&s->top_level_entry.free_rows);

  vec_free(&s->body_scope_rows);
  vec_free(&s->body_scope_binds);
  vec_free(&s->aggregate_field_pool);
  vec_free(&s->enum_variant_pool);
  vec_free(&s->namespace_field_pool);

#define X(name, type) vec_free(&s->scopes.name);
  ORE_SCOPES_COLUMNS(X)
#undef X
  vec_free(&s->scopes.decl_pool);

  // query_stack lives in s->arena and is reclaimed by arena_free in
  // db_free. running_slots is malloc-backed engine state, freed by
  // db_engine_free which ran before this.
}
