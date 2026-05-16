#include "ids.h"
#include "../db.h"

#include <assert.h>
#include <string.h>

// First-chunk capacity for a per-module arena (db.modules.arenas[mid]).
// Modest: most modules are small; large ones grow via chunk doubling.
// Was ORE_MODULE_ARENA_DEFAULT_CAP in the now-deleted module_info.c.
#define ORE_MODULE_ARENA_DEFAULT_CAP (16 * 1024)

// =============================================================================
// SoA column initialization.
//
// Every column in db.defs / db.scopes / db.sources / db.modules /
// db.query_stack is a malloc-backed Vec (vec_init). These are long-lived, grow
// over the session, and would leak memory if backed by the arena (every
// doubling would orphan the prior buffer with no reclaim path).
//
// Slot 0 of every "id-indexed" column is reserved as the NONE sentinel — push
// a zero row so DefId(0) / ScopeId(0) / SourceId(0) / ModuleId(0) all map to
// a defined but-empty row. query_stack is a true stack (no NONE convention),
// so it stays count=0 at init.
// =============================================================================

void db_ids_init(struct db *s) {
  /* ---- Sources / modules ----------------------------------------------- */

  // sources SoA
  vec_init(&s->sources.hashes, sizeof(uint64_t));
  vec_init(&s->sources.versions, sizeof(uint32_t));
  vec_init(&s->sources.paths, sizeof(StrId));
  vec_init(&s->sources.texts, sizeof(char *));
  vec_init(&s->sources.text_lens, sizeof(uint32_t));

  vec_push_zero(&s->sources.hashes);
  vec_push_zero(&s->sources.versions);
  vec_push_zero(&s->sources.paths);
  vec_push_zero(&s->sources.texts);
  vec_push_zero(&s->sources.text_lens);

  // modules SoA
  vec_init(&s->modules.ids, sizeof(ModuleId));
  vec_init(&s->modules.names, sizeof(StrId));
  vec_init(&s->modules.files, sizeof(FileId));
  vec_init(&s->modules.durable_fps, sizeof(Fingerprint));
  vec_init(&s->modules.line_starts, sizeof(Vec));
  vec_init(&s->modules.node_data, sizeof(ModuleNodeData));
  vec_init(&s->modules.node_counts, sizeof(uint32_t));
  vec_init(&s->modules.arenas, sizeof(Arena));
  vec_init(&s->modules.asts, sizeof(void *));
  vec_init(&s->modules.trivia_tokens, sizeof(Vec));
  vec_init(&s->modules.trivia_offsets, sizeof(Vec));
  vec_init(&s->modules.ast_id_maps, sizeof(void *));
  vec_init(&s->modules.top_level_indices, sizeof(Vec));
  vec_init(&s->modules.node_to_decls, sizeof(Vec));
  vec_init(&s->modules.slots_ast, sizeof(struct QuerySlot));
  vec_init(&s->modules.slots_index, sizeof(struct QuerySlot));
  vec_init(&s->modules.slots_exports, sizeof(struct QuerySlot));

  vec_push_zero(&s->modules.ids);
  vec_push_zero(&s->modules.names);
  vec_push_zero(&s->modules.files);
  vec_push_zero(&s->modules.durable_fps);
  vec_push_zero(&s->modules.line_starts); // NONE module: empty inner Vec
  vec_push_zero(&s->modules.node_data);
  vec_push_zero(&s->modules.node_counts);
  vec_push_zero(
      &s->modules.arenas); // NONE module: arena stays zeroed (never parsed)
  vec_push_zero(&s->modules.asts);
  vec_push_zero(&s->modules.trivia_tokens);
  vec_push_zero(&s->modules.trivia_offsets);
  vec_push_zero(&s->modules.ast_id_maps);
  vec_push_zero(&s->modules.top_level_indices);
  vec_push_zero(&s->modules.node_to_decls);
  vec_push_zero(&s->modules.slots_ast);
  vec_push_zero(&s->modules.slots_index);
  vec_push_zero(&s->modules.slots_exports);

  /* ---- defs SoA -------------------------------------------------------- */

  // Identity columns (durable across reparses).
  vec_init(&s->defs.names, sizeof(StrId));
  vec_init(&s->defs.kinds, sizeof(DefKind));
  vec_init(&s->defs.meta, sizeof(DefMeta));
  vec_init(&s->defs.ast_ids, sizeof(AstId));
  vec_init(&s->defs.owner_scopes, sizeof(ScopeId));
  vec_init(&s->defs.parent_modules, sizeof(ModuleId));

  // Per-decl durable fingerprint (R6).
  vec_init(&s->defs.durable_fps, sizeof(Fingerprint));

  // Cached query outputs.
  vec_init(&s->defs.types, sizeof(IpIndex));
  vec_init(&s->defs.values, sizeof(IpIndex));
  vec_init(&s->defs.effect_sigs, sizeof(IpIndex));

  vec_init(&s->defs.slots_type, sizeof(struct QuerySlot));
  vec_init(&s->defs.slots_signature, sizeof(struct QuerySlot));
  vec_init(&s->defs.slots_const_eval, sizeof(struct QuerySlot));

  // Seed slot 0 = DEF_ID_NONE across every defs column.
  vec_push_zero(&s->defs.names);
  vec_push_zero(&s->defs.kinds);
  vec_push_zero(&s->defs.meta);
  vec_push_zero(&s->defs.ast_ids);
  vec_push_zero(&s->defs.owner_scopes);
  vec_push_zero(&s->defs.parent_modules);
  vec_push_zero(&s->defs.durable_fps);
  vec_push_zero(&s->defs.types);
  vec_push_zero(&s->defs.values);
  vec_push_zero(&s->defs.effect_sigs);
  vec_push_zero(&s->defs.slots_type);
  vec_push_zero(&s->defs.slots_signature);
  vec_push_zero(&s->defs.slots_const_eval);

  /* ---- scopes SoA ------------------------------------------------------ */

  vec_init(&s->scopes.parents, sizeof(ScopeId));
  vec_init(&s->scopes.meta, sizeof(ScopeMeta));
  vec_init(&s->scopes.owning_modules, sizeof(ModuleId));
  vec_init(&s->scopes.decl_offsets, sizeof(uint32_t));
  vec_init(&s->scopes.decl_pool, sizeof(DeclEntry));
  vec_init(&s->scopes.slots_resolve_ref, sizeof(struct QuerySlot));

  // Seed ScopeId(0) = NONE. decl_offsets needs two entries (start +
  // sentinel-end) for the NONE scope to have a well-formed empty range.
  vec_push_zero(&s->scopes.parents);
  vec_push_zero(&s->scopes.meta);
  vec_push_zero(&s->scopes.owning_modules);
  vec_push_zero(&s->scopes.decl_offsets); // start of NONE scope's range
  vec_push_zero(&s->scopes.decl_offsets); // sentinel: end of NONE scope's range
  vec_push_zero(&s->scopes.slots_resolve_ref);

  /* ---- query stack ----------------------------------------------------- */

  // Arena-backed fixed-capacity: 256 frames covers real-world nesting with
  // plenty of breathing room, and overflow asserts at the call site
  // rather than triggering a silent realloc that would invalidate any
  // QueryFrame pointer held across a nested db_query_begin. The query
  // engine assumes the stack never relocates — see query.c.
  vec_init_in_arena(&s->query_stack, &s->arena, 256, sizeof(struct QueryFrame));
}

// Reserve a fresh DefId. Every defs column grows by one zero row in
// lockstep so DefId(N) names row N everywhere.
DefId db_alloc_def(struct db *s) {
  uint32_t idx = (uint32_t)s->defs.names.count;

  vec_push_zero(&s->defs.names);
  vec_push_zero(&s->defs.kinds);
  vec_push_zero(&s->defs.meta);
  vec_push_zero(&s->defs.ast_ids);
  vec_push_zero(&s->defs.owner_scopes);
  vec_push_zero(&s->defs.parent_modules);
  vec_push_zero(&s->defs.durable_fps);
  vec_push_zero(&s->defs.types);
  vec_push_zero(&s->defs.values);
  vec_push_zero(&s->defs.effect_sigs);
  vec_push_zero(&s->defs.slots_type);
  vec_push_zero(&s->defs.slots_signature);
  vec_push_zero(&s->defs.slots_const_eval);

  return (DefId){.idx = idx};
}

// Reserve a fresh ScopeId. Every scopes column grows by one zero row;
// the decl_offsets sentinel is updated so the new scope starts with a
// well-formed empty decl range [decl_pool.count, decl_pool.count).
ScopeId db_alloc_scope(struct db *s) {
  uint32_t idx = (uint32_t)s->scopes.parents.count;

  vec_push_zero(&s->scopes.parents);
  vec_push_zero(&s->scopes.meta);
  vec_push_zero(&s->scopes.owning_modules);
  vec_push_zero(&s->scopes.slots_resolve_ref);

  // The new scope inherits the current decl_pool end as its start.
  // We push one new offset entry (the sentinel that marks the END of
  // the new scope's range). The previous sentinel now serves as the
  // start of the new scope's range. Invariant maintained:
  //     decl_offsets.count == scope_count + 1
  uint32_t end_offset = (uint32_t)s->scopes.decl_pool.count;
  vec_push(&s->scopes.decl_offsets, &end_offset);

  return (ScopeId){.idx = idx};
}

ModuleId db_alloc_module(struct db *s) {
  uint32_t idx = (uint32_t)s->modules.names.count;
  ModuleId mid = {.idx = idx};
  vec_push(&s->modules.ids, &mid);
  vec_push_zero(&s->modules.names);
  vec_push_zero(&s->modules.files);
  vec_push_zero(&s->modules.durable_fps);
  vec_push_zero(&s->modules.line_starts); // empty inner Vec — lexer initializes
  vec_push_zero(&s->modules.node_data);
  vec_push_zero(&s->modules.node_counts);
  vec_push_zero(&s->modules.arenas);
  arena_init((Arena *)vec_get(&s->modules.arenas, idx),
             ORE_MODULE_ARENA_DEFAULT_CAP);
  vec_push_zero(&s->modules.asts);
  vec_push_zero(&s->modules.trivia_tokens);
  vec_push_zero(&s->modules.trivia_offsets);
  vec_push_zero(&s->modules.ast_id_maps);
  vec_push_zero(&s->modules.top_level_indices);
  vec_push_zero(&s->modules.node_to_decls);
  vec_push_zero(&s->modules.slots_ast);
  vec_push_zero(&s->modules.slots_index);
  vec_push_zero(&s->modules.slots_exports);

  return (ModuleId){.idx = idx};
}

ModuleId db_module_for_file(struct db *s, FileId file) {
  if (!file_id_valid(file))
    return MODULE_ID_NONE;

  for (size_t i = 1; i < s->modules.files.count; i++) {
    FileId *fid = (FileId *)vec_get(&s->modules.files, i);
    if (file_id_eq(*fid, file))
      return (ModuleId){.idx = (uint32_t)i};
  }
  return MODULE_ID_NONE;
}

// FNV-1a 64-bit over a byte buffer. Used for source content hashing so
// the LSP can detect "nothing actually changed" without re-parsing.
// Inline rather than depending on query_engine.h for one helper.
static uint64_t source_fnv1a(const char *data, size_t len) {
  uint64_t h = 0xcbf29ce484222325ULL; // FNV offset basis (64-bit)
  for (size_t i = 0; i < len; i++) {
    h ^= (uint8_t)data[i];
    h *= 0x100000001b3ULL; // FNV prime (64-bit)
  }
  return h;
}

AstId ast_id_compute(uint32_t kind, StrId name) {
  uint64_t h = 0xcbf29ce484222325ULL;

  // Hash kind
  h ^= (uint64_t)kind;
  h *= 0x100000001b3ULL;

  // Hash name
  h ^= (uint64_t)name.idx;
  h *= 0x100000001b3ULL;

  return (AstId){.idx = (uint32_t)(h ^ (h >> 32))};
}

SourceId db_alloc_source(struct db *s, const char *path, size_t path_len,
                         const char *text, size_t text_len) {
  // TinySpan packs the byte offset into 24 bits. A source file > 16MB
  // would silently wrap and produce wrong spans across the entire
  // file. Catch it loudly here; if we need bigger files in the
  // future, widen TinySpan rather than dropping this check.
  assert(text_len < (1u << 24) && "source > 16MB exceeds TinySpan range");

  uint32_t idx = (uint32_t)s->sources.hashes.count;

  // Pre-grow the intern slot table for this source's identifiers before
  // parsing touches it — avoids ~log2(N) doubling/rehash passes. Dense
  // Ore averages roughly one interned identifier per ~8 source bytes.
  pool_reserve_slots(&s->strings, text_len / 8);

  StrId path_id = pool_intern(&s->strings, path, path_len);

  char *text_copy = (char *)arena_alloc_raw(&s->arena, text_len + 1);
  if (text_len)
    memcpy(text_copy, text, text_len);
  text_copy[text_len] = '\0';

  uint64_t hash = source_fnv1a(text, text_len);
  uint32_t version = 1;

  vec_push(&s->sources.hashes, &hash);
  vec_push(&s->sources.versions, &version);
  vec_push(&s->sources.paths, &path_id);
  vec_push(&s->sources.texts, &text_copy);
  vec_push(&s->sources.text_lens, &text_len);

  return (SourceId){.idx = idx};
}

// Free all malloc-backed Vec storage on the database. Called from db_free.
void db_ids_free(struct db *s) {
  vec_free(&s->sources.hashes);
  vec_free(&s->sources.versions);
  vec_free(&s->sources.paths);
  vec_free(&s->sources.texts);
  vec_free(&s->sources.text_lens);

  // Per-module teardown. The reparse path frees the PRIOR generation's
  // malloc Vecs; the LIVE generation is only reclaimed here. Free the
  // malloc-backed per-module buffers BEFORE the per-module arena (the
  // ASTStore struct lives in that arena). vec_free is idempotent on the
  // zeroed slots of never-parsed modules (e.g. idx 0 NONE).
  for (size_t i = 0; i < s->modules.ids.count; i++) {
    // Opaque to db core — ast_store_free lives with the parser (owner of
    // the ASTStore layout); we only need the forward decl + the pointer.
    struct ASTStore;
    extern void ast_store_free(struct ASTStore *);
    ast_store_free(*(struct ASTStore **)vec_get(&s->modules.asts, i));
    vec_free((Vec *)vec_get(&s->modules.top_level_indices, i));
    vec_free((Vec *)vec_get(&s->modules.node_to_decls, i));
    vec_free((Vec *)vec_get(&s->modules.line_starts, i));
    vec_free((Vec *)vec_get(&s->modules.trivia_tokens, i));
    vec_free((Vec *)vec_get(&s->modules.trivia_offsets, i));
  }

  // Per-module arenas: free each (reclaims the ASTStore struct + the
  // flattened ModuleNodeData block). idx 0 (NONE) is zeroed and never
  // arena_init'd; arena_free is NULL-safe.
  for (size_t i = 0; i < s->modules.arenas.count; i++) {
    arena_free((Arena *)vec_get(&s->modules.arenas, i));
  }
  vec_free(&s->modules.arenas);

  vec_free(&s->modules.names);
  vec_free(&s->modules.files);
  vec_free(&s->modules.durable_fps);
  vec_free(&s->modules.line_starts);
  vec_free(&s->modules.node_data);
  vec_free(&s->modules.node_counts);
  vec_free(&s->modules.ids);
  vec_free(&s->modules.asts);
  vec_free(&s->modules.trivia_tokens);
  vec_free(&s->modules.trivia_offsets);
  vec_free(&s->modules.ast_id_maps);
  vec_free(&s->modules.top_level_indices);
  vec_free(&s->modules.node_to_decls);
  vec_free(&s->modules.slots_ast);
  vec_free(&s->modules.slots_index);
  vec_free(&s->modules.slots_exports);

  vec_free(&s->defs.names);
  vec_free(&s->defs.kinds);
  vec_free(&s->defs.meta);
  vec_free(&s->defs.ast_ids);
  vec_free(&s->defs.owner_scopes);
  vec_free(&s->defs.parent_modules);
  vec_free(&s->defs.durable_fps);
  vec_free(&s->defs.types);
  vec_free(&s->defs.values);
  vec_free(&s->defs.effect_sigs);
  vec_free(&s->defs.slots_type);
  vec_free(&s->defs.slots_signature);
  vec_free(&s->defs.slots_const_eval);

  vec_free(&s->scopes.parents);
  vec_free(&s->scopes.meta);
  vec_free(&s->scopes.owning_modules);
  vec_free(&s->scopes.decl_offsets);
  vec_free(&s->scopes.decl_pool);
  vec_free(&s->scopes.slots_resolve_ref);

  vec_free(&s->query_stack);
}
