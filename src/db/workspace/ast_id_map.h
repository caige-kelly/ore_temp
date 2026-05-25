#ifndef ORE_DB_WORKSPACE_AST_ID_MAP_H
#define ORE_DB_WORKSPACE_AST_ID_MAP_H

#include "../ids/ids.h"          // AstId, AST_ID_NONE, AstNodeId, StrId
#include "../../support/data_structure/arena.h"
#include "../../support/data_structure/hashmap.h"

/*
    Per-module AstIdMap.

    AstId is the reparse-stable identity handle for a top-level AST item.
    Where AstNodeId is position-based (shifts on reorder), AstId is
    derived from (item kind, name) — adding a sibling decl earlier in
    the file does NOT shift any other item's AstId. This is the
    invariant DefId allocation relies on (DefId is keyed off AstId).

    Mirrors rust-analyzer's FileAstId<T> design.

    Identity: hash((kind, name_id)). Collisions are resolved by linear
    probing — the AstId value is the slot the item landed in, not the
    raw hash. Walk order during build is the parser's emit order (source
    order), which is deterministic per-parse.

    Scope: per-module. Two modules can each have an item with AstId
    0x1234 — those are different identities, qualified by NamespaceId.

    Storage: HashMap maps AstId.idx (u32) → AstNodeId.idx (u32 packed
    into the void* value slot via uintptr_t). The HashMap is
    arena-backed against the owning ModuleInfo's per-module arena.
    `ast_id_map_reset` is called at the start of each (re)compute of
    QUERY_TOP_LEVEL_INDEX so stale AstNodeIds from the prior revision
    don't linger.
*/

struct AstIdMap {
    HashMap id_to_node;  // AstId.idx → AstNodeId.idx (packed in value ptr)
};

// Initialize an empty map. Storage backing comes from `arena` —
// typically the owning ModuleInfo's per-module arena, so the map's
// HashMap buckets share the parse-pass lifetime.
void ast_id_map_init(struct AstIdMap *map, Arena *arena);

// Drop all entries. Buckets stay allocated for reuse. Called at the
// start of each (re)compute so the map describes the current
// revision only.
void ast_id_map_reset(struct AstIdMap *map);

// Insert an item, returning its assigned AstId. Computes the hash
// from (kind, name_id) via `ast_id_compute` and probes forward on
// collision. Re-inserting the same logical item (same kind + name) at
// a different AstNodeId updates the binding in place and returns the
// same AstId.
//
// `kind` is interpreted as `AstNodeKind` (parser-side identity) — kept
// as `uint32_t` here so this header stays layering-clean (no
// parser/ast.h include from db/workspace).
//
// Returns AST_ID_NONE if `node` is the NONE sentinel (refused).
AstId ast_id_map_insert(struct AstIdMap *map, uint32_t kind, StrId name_id,
                        AstNodeId node);

// Look up the AstNodeId for an AstId. Returns AST_NODE_ID_NONE if the
// AstId doesn't correspond to a current entry (e.g., the item was
// removed in this revision).
AstNodeId ast_id_map_get(struct AstIdMap *map, AstId id);

#endif // ORE_DB_WORKSPACE_AST_ID_MAP_H
