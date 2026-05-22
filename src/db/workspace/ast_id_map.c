#include "ast_id_map.h"

#include <stdint.h>

// Canonical (kind, name) → u32 mix. Delegates to `ast_id_compute` so
// the map's keys match the AstId stamped on each TopLevelEntry by the
// parser. Sentinel guard: if the canonical hash lands on 0 (rare —
// would mean AST_ID_NONE collision), bump to 1.
static uint32_t hash_kind_name(uint32_t kind, StrId name_id) {
  AstId id = ast_id_compute(kind, name_id);
  return id.idx == 0 ? 1u : id.idx;
}

void ast_id_map_init(struct AstIdMap *map, Arena *arena) {
  if (!map)
    return;
  hashmap_init_in(&map->id_to_node, arena);
}

void ast_id_map_reset(struct AstIdMap *map) {
  if (!map)
    return;
  hashmap_clear(&map->id_to_node);
}

AstId ast_id_map_insert(struct AstIdMap *map, uint32_t kind, StrId name_id,
                        AstNodeId node) {
  if (!map || !ast_node_id_valid(node))
    return AST_ID_NONE;

  // Canonical AstId — hash(kind, name) via ast_id_compute, with the
  // 0-sentinel guard. The hashmap layer handles its own collision
  // resolution (re-hashes the key via hash_u64 and probes internally),
  // so we just `put` and let it do its job — guaranteed O(1) amortized
  // regardless of input shape. The previous version did its OWN probe
  // loop on top of the hashmap's probing, which (a) double-counted
  // collision work and (b) degenerated to O(N) per insert if the
  // module had duplicate (kind, name) decls (a sema error, but the
  // parser used to amplify it into quadratic time).
  //
  // Idempotent put semantics: `hashmap_put` replaces the value at an
  // existing key. Duplicate (kind, name) → same AstId → second insert
  // overrides the first AstNodeId. That's correct: AstId is the
  // canonical identity, and sema detects the duplicate separately via
  // its own top-level-index pass.
  uint32_t h = hash_kind_name(kind, name_id);
  void *value = (void *)(uintptr_t)node.idx;
  hashmap_put_or_die(&map->id_to_node, (uint64_t)h, value, "ast_id_map");
  return (AstId){.idx = h};
}

AstNodeId ast_id_map_get(struct AstIdMap *map, AstId id) {
  if (!map || !ast_id_valid(id) ||
      !hashmap_is_initialized(&map->id_to_node)) {
    return AST_NODE_ID_NONE;
  }
  if (!hashmap_contains(&map->id_to_node, (uint64_t)id.idx)) {
    return AST_NODE_ID_NONE;
  }
  void *value = hashmap_get(&map->id_to_node, (uint64_t)id.idx);
  return (AstNodeId){.idx = (uint32_t)(uintptr_t)value};
}
