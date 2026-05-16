#include "ast_id_map.h"

#include <stdint.h>

// FNV-1a-style mix over (kind, name_id). Constants aren't load-bearing
// — what matters is determinism (same input → same hash on every parse)
// and a sensible distribution to keep collisions rare.
//
// We avoid hash==0 because AST_ID_NONE.idx == 0 is the invalid sentinel.
// If the hash lands on 0, bump to 1.
static uint32_t hash_kind_name(DefKind kind, StrId name_id) {
  uint32_t h = 2166136261u; // FNV offset basis (32-bit)
  h ^= (uint32_t)kind;
  h *= 16777619u; // FNV prime (32-bit)
  h ^= name_id.idx;
  h *= 16777619u;
  return h == 0 ? 1u : h;
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

AstId ast_id_map_insert(struct AstIdMap *map, DefKind kind, StrId name_id,
                        AstNodeId node) {
  if (!map)
    return AST_ID_NONE;
  if (!ast_node_id_valid(node))
    return AST_ID_NONE;

  // Canonical hash; probe forward on collision until we find a free
  // slot OR an existing slot owned by the same (kind, name) identity
  // (which we detect via a recompute-stable hash match). Probing
  // within a single build pass produces deterministic AstIds — same
  // source order → same walk order → same probe outcomes.
  uint32_t h = hash_kind_name(kind, name_id);
  for (uint32_t probe = 0; probe < UINT32_MAX; probe++) {
    uint32_t slot = h + probe;
    if (slot == 0)
      continue; // 0 is the invalid sentinel

    // Pack AstNodeId.idx into a void* through uintptr_t. AstNodeId
    // is u32; uintptr_t is at least 32 bits on every platform we
    // support (and 64 on macOS/linux). Round-trip in get().
    void *value = (void *)(uintptr_t)node.idx;

    if (map->id_to_node.entries != NULL &&
        hashmap_contains(&map->id_to_node, (uint64_t)slot)) {
      // Slot is occupied. We don't have a way to tell whether
      // it's "us" (same kind+name, re-insert with new node) or a
      // collision from a different (kind, name) without storing
      // the kind/name alongside. Today the only re-insert path
      // is "same parse, same item" which produces the same probe
      // sequence — so the first free slot we find IS our slot
      // every time. Just keep probing.
      continue;
    }
    hashmap_put_or_die(&map->id_to_node, (uint64_t)slot, value, "ast_id_map");
    return (AstId){.idx = slot};
  }
  return AST_ID_NONE; // unreachable in practice
}

AstNodeId ast_id_map_get(struct AstIdMap *map, AstId id) {
  if (!map || !ast_id_valid(id) || map->id_to_node.entries == NULL) {
    return AST_NODE_ID_NONE;
  }
  if (!hashmap_contains(&map->id_to_node, (uint64_t)id.idx)) {
    return AST_NODE_ID_NONE;
  }
  void *value = hashmap_get(&map->id_to_node, (uint64_t)id.idx);
  return (AstNodeId){.idx = (uint32_t)(uintptr_t)value};
}
