#include "ids.h"
#include "../db.h"

#include <stdlib.h>

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
  vec_init(&s->files.line_starts, sizeof(Vec));
  vec_init(&s->files.node_data, sizeof(ModuleNodeData));
  vec_init(&s->files.node_counts, sizeof(uint32_t));
  vec_init(&s->files.arenas, sizeof(Arena));
  vec_init(&s->files.asts, sizeof(void *));
  vec_init(&s->files.trivia_tokens, sizeof(Vec));
  vec_init(&s->files.trivia_offsets, sizeof(Vec));
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

  /* ---- defs SoA -------------------------------------------------------- */

#define X(name, type) vec_init(&s->defs.name, sizeof(type));
  ORE_DEFS_COLUMNS(X)
#undef X
  vec_init(&s->defs.body_scopes, sizeof(BodyScopes *));

#define X(name, type) vec_push_zero(&s->defs.name);
  ORE_DEFS_COLUMNS(X)
#undef X
  vec_push_zero(&s->defs.body_scopes);

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
  vec_push_zero(&s->defs.body_scopes);

  return (DefId){.idx = idx};
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
    vec_free((Vec *)vec_get(&s->files.line_starts, i));
    vec_free((Vec *)vec_get(&s->files.trivia_tokens, i));
    vec_free((Vec *)vec_get(&s->files.trivia_offsets, i));
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
  // body_scopes: outer Vec stores BodyScopes* pointers; each pointed-to
  // struct is arena-allocated, but its internal backing buffers
  // (scopes Vec, binds Vec, node_to_scope array) are malloc-owned and
  // must be freed individually before the outer Vec.
  for (size_t i = 0; i < s->defs.body_scopes.count; i++) {
    BodyScopes **slot = (BodyScopes **)vec_get(&s->defs.body_scopes, i);
    if (*slot) {
      vec_free(&(*slot)->scopes);
      vec_free(&(*slot)->binds);
      free((*slot)->node_to_scope);
    }
  }
  vec_free(&s->defs.body_scopes);

  vec_free(&s->scopes.parents);
  vec_free(&s->scopes.meta);
  vec_free(&s->scopes.owning_modules);
  vec_free(&s->scopes.decl_offsets);
  vec_free(&s->scopes.decl_pool);

  vec_free(&s->query_stack);
}
