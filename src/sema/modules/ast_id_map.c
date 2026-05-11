#include "ast_id_map.h"

#include "../sema.h"

// Hash function for `(kind, name)`. FNV-1a-style mix tuned for the
// expected key space (DeclKind is a small enum, name_id is a u32
// pool index). The exact constants aren't load-bearing — what
// matters is determinism (same input → same hash on every parse)
// and a sensible distribution to keep collisions rare.
//
// We avoid 0 because AST_ID_NONE = 0 is the invalid sentinel; if
// the hash happens to land on 0, bump to 1.
static uint32_t hash_kind_name(DeclKind kind, StrId name_id) {
  uint32_t h = 2166136261u; // FNV offset basis
  h ^= (uint32_t)kind;
  h *= 16777619u;           // FNV prime
  h ^= name_id.v;
  h *= 16777619u;
  return h == 0 ? 1u : h;
}

void ast_id_map_init(struct AstIdMap *map, struct Sema *s) {
  hashmap_init_in(&map->id_to_node, &s->arena);
}

void ast_id_map_reset(struct AstIdMap *map) {
  hashmap_clear(&map->id_to_node);
}

AstId ast_id_map_insert(struct AstIdMap *map, DeclKind kind, StrId name_id,
                        struct Expr *node) {
  if (!map || !node)
    return AST_ID_NONE;

  // `hashmap_put_or_die` lazy-allocates the entries array on the
  // first call — the map can validly be in the "uninitialized
  // backing storage" state (entries==NULL) before any insert. We do
  // NOT short-circuit on that. The arena pointer was stashed at
  // ast_id_map_init time so the lazy alloc has somewhere to grab
  // memory.
  //
  // Canonical hash; probe forward on collision until we find a free
  // slot. Probing within a single build pass produces deterministic
  // AstIds — same source → same walk order → same probe outcomes.
  uint32_t h = hash_kind_name(kind, name_id);
  for (uint32_t probe = 0; probe < UINT32_MAX; probe++) {
    uint32_t slot = h + probe;
    if (slot == 0)
      continue; // 0 is the invalid sentinel
    if (map->id_to_node.entries != NULL &&
        hashmap_contains(&map->id_to_node, (uint64_t)slot))
      continue; // collision — try next slot
    hashmap_put_or_die(&map->id_to_node, (uint64_t)slot, node, "ast_id_map");
    return (AstId){slot};
  }
  return AST_ID_NONE; // unreachable in practice
}

struct Expr *ast_id_map_get(struct AstIdMap *map, AstId id) {
  if (!map || !ast_id_is_valid(id) || map->id_to_node.entries == NULL)
    return NULL;
  if (!hashmap_contains(&map->id_to_node, (uint64_t)id.v))
    return NULL;
  return (struct Expr *)hashmap_get(&map->id_to_node, (uint64_t)id.v);
}
