#include "ids.h"
#include "../db.h"

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
  /* ---- Sources / modules ----------------------------------------------- */

  vec_init(&s->sources.hashes, sizeof(uint64_t));
  vec_init(&s->sources.versions, sizeof(uint32_t));
  vec_init(&s->sources.paths, sizeof(StrId));
  vec_init(&s->sources.texts, sizeof(char *));
  vec_init(&s->sources.text_lens, sizeof(uint32_t));
  vec_init(&s->sources.durability, sizeof(Durability));

  vec_push_zero(&s->sources.hashes);
  vec_push_zero(&s->sources.versions);
  vec_push_zero(&s->sources.paths);
  vec_push_zero(&s->sources.texts);
  vec_push_zero(&s->sources.text_lens);
  vec_push_zero(&s->sources.durability);

  // files SoA — the per-file parse unit (QUERY_FILE_AST keyed here).
  vec_init(&s->files.ids, sizeof(FileId));
  vec_init(&s->files.source_id, sizeof(SourceId));
  vec_init(&s->files.module_id, sizeof(ModuleId));
  vec_init(&s->files.line_starts, sizeof(FileArray));
  vec_init(&s->files.node_data, sizeof(ModuleNodeData));
  vec_init(&s->files.node_counts, sizeof(uint32_t));
  vec_init(&s->files.arenas, sizeof(Arena));
  vec_init(&s->files.asts, sizeof(void *));
  vec_init(&s->files.trivia_tokens, sizeof(FileArray));
  vec_init(&s->files.trivia_offsets, sizeof(FileArray));
  vec_init(&s->files.ast_id_maps, sizeof(void *));
  vec_init(&s->files.top_level_indices, sizeof(Vec));
  vec_init(&s->files.slots_ast, sizeof(struct QuerySlot));

  vec_push_zero(&s->files.ids);
  vec_push_zero(&s->files.source_id);
  vec_push_zero(&s->files.module_id);
  vec_push_zero(&s->files.line_starts);
  vec_push_zero(&s->files.node_data);
  vec_push_zero(&s->files.node_counts);
  vec_push_zero(&s->files.arenas);
  vec_push_zero(&s->files.asts);
  vec_push_zero(&s->files.trivia_tokens);
  vec_push_zero(&s->files.trivia_offsets);
  vec_push_zero(&s->files.ast_id_maps);
  vec_push_zero(&s->files.top_level_indices);
  vec_push_zero(&s->files.slots_ast);

  // modules SoA.
  vec_init(&s->modules.ids, sizeof(ModuleId));
  vec_init(&s->modules.names, sizeof(StrId));
  vec_init(&s->modules.file_offsets, sizeof(uint32_t));
  vec_init(&s->modules.file_pool, sizeof(FileId));
  vec_init(&s->modules.exports, sizeof(ScopeId));
  vec_init(&s->modules.internal_scopes, sizeof(ScopeId));
  vec_init(&s->modules.slots_index, sizeof(struct QuerySlot));
  vec_init(&s->modules.slots_exports, sizeof(struct QuerySlot));

  vec_push_zero(&s->modules.ids);
  vec_push_zero(&s->modules.names);
  vec_push_zero(&s->modules.file_offsets);
  vec_push_zero(&s->modules.file_offsets);
  vec_push_zero(&s->modules.exports);
  vec_push_zero(&s->modules.internal_scopes);
  vec_push_zero(&s->modules.slots_index);
  vec_push_zero(&s->modules.slots_exports);

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

  // Body-scope pools — pure append arrays, range-addressed.
  vec_init(&s->body_scope_rows, sizeof(ScopeRow));
  vec_init(&s->body_scope_binds, sizeof(ScopedBind));
  vec_init(&s->node_to_scope, sizeof(uint32_t));

  /* ---- scopes SoA ------------------------------------------------------ */

  vec_init(&s->scopes.parents, sizeof(ScopeId));
  vec_init(&s->scopes.meta, sizeof(ScopeMeta));
  vec_init(&s->scopes.owning_modules, sizeof(ModuleId));
  vec_init(&s->scopes.decl_offsets, sizeof(uint32_t));
  vec_init(&s->scopes.decl_pool, sizeof(DeclEntry));

  vec_push_zero(&s->scopes.parents);
  vec_push_zero(&s->scopes.meta);
  vec_push_zero(&s->scopes.owning_modules);
  vec_push_zero(&s->scopes.decl_offsets);
  vec_push_zero(&s->scopes.decl_offsets);

  /* ---- query stack ----------------------------------------------------- */

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
ScopeId db_create_scope(struct db *s) {
  uint32_t idx = (uint32_t)s->scopes.parents.count;

  vec_push_zero(&s->scopes.parents);
  vec_push_zero(&s->scopes.meta);
  vec_push_zero(&s->scopes.owning_modules);

  uint32_t end_offset = (uint32_t)s->scopes.decl_pool.count;
  vec_push(&s->scopes.decl_offsets, &end_offset);

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

  vec_free(&s->sources.hashes);
  vec_free(&s->sources.versions);
  vec_free(&s->sources.paths);
  vec_free(&s->sources.texts);
  vec_free(&s->sources.text_lens);
  vec_free(&s->sources.durability);

  for (size_t i = 0; i < s->files.ids.count; i++) {
    struct ASTStore;
    extern void ast_store_free(struct ASTStore *);
    ast_store_free(*(struct ASTStore **)vec_get(&s->files.asts, i));
    vec_free((Vec *)vec_get(&s->files.top_level_indices, i));
    // line_starts / trivia_* are FileArrays whose data lives in this
    // file's arena — reclaimed by the arena_free below.
    arena_free((Arena *)vec_get(&s->files.arenas, i));
  }
  vec_free(&s->files.ids);
  vec_free(&s->files.source_id);
  vec_free(&s->files.module_id);
  vec_free(&s->files.line_starts);
  vec_free(&s->files.node_data);
  vec_free(&s->files.node_counts);
  vec_free(&s->files.arenas);
  vec_free(&s->files.asts);
  vec_free(&s->files.trivia_tokens);
  vec_free(&s->files.trivia_offsets);
  vec_free(&s->files.ast_id_maps);
  vec_free(&s->files.top_level_indices);
  vec_free(&s->files.slots_ast);

  vec_free(&s->modules.ids);
  vec_free(&s->modules.names);
  vec_free(&s->modules.file_offsets);
  vec_free(&s->modules.file_pool);
  vec_free(&s->modules.exports);
  vec_free(&s->modules.internal_scopes);
  vec_free(&s->modules.slots_index);
  vec_free(&s->modules.slots_exports);

#define X(name, type) vec_free(&s->defs.name);
  ORE_DEFS_COLUMNS(X)
#undef X
  // Per-kind tables. Each slot column's per-slot deps buffers were
  // already released by slot_release_visitor (db_for_each_slot, run from
  // db_free before db_ids_free); here we free the column buffers.
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
  vec_free(&s->body_scope_rows);
  vec_free(&s->body_scope_binds);
  vec_free(&s->node_to_scope);

  vec_free(&s->scopes.parents);
  vec_free(&s->scopes.meta);
  vec_free(&s->scopes.owning_modules);
  vec_free(&s->scopes.decl_offsets);
  vec_free(&s->scopes.decl_pool);

  vec_free(&s->query_stack);
}
