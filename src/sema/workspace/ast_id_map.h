#ifndef ORE_SEMA_AST_ID_MAP_H
#define ORE_SEMA_AST_ID_MAP_H

#include <stdbool.h>
#include <stdint.h>

#include "../../support/common/hashmap.h"
#include "../../support/common/stringpool.h"
#include "../../db/ids/ids.h"     // AstId, AST_ID_NONE
#include "../name_resolution/scope/scope.h" // DeclKind

struct Sema;
struct Expr;

// The `AstId` type itself lives in `sema/ids/ids.h` next to DefId /
// ScopeId / ModuleId. This header owns the map machinery.
//
// AstId design: stable identity for an item across reparses. Mirrors
// rust-analyzer's `FileAstId<T>` ([span/src/ast_id.rs]). The
// invariant: for a given source file, the same (item kind, name)
// tuple produces the same AstId every parse, regardless of byte
// position. Inserting a sibling decl earlier in the file does NOT
// shift any other item's AstId — that's the property that
// position-based NodeIds lacked.
//
// Identity is computed from `hash((kind, name_id))`. Collisions are
// resolved by linear probing — the AstId value is the slot the item
// landed in, NOT the raw hash. Walk order during build is the
// top-level index's natural source order, which is deterministic
// per-parse (the parser emits items in source order).
//
// Scope: per-module. Two different modules can each have an item
// with AstId 0x1234 — those are different identities, qualified by
// the owning module. Same shape as rust-analyzer's `FileAstId<T>`
// being scoped to its `HirFileId`.

// Per-module AstIdMap. Hash table from AstId → Expr* (the originating
// AST node, typically an expr_Bind). The Expr* is borrowed — its
// lifetime is the parser arena, which is owned by Sema and refreshed
// on every reparse. `ast_id_map_reset` is called at the start of
// each rebuild (inside query_top_level_index) so stale Expr*s from
// the prior revision don't linger.
//
// The forward lookup `Expr* → AstId` is intentionally not provided —
// today, every consumer that wants an AstId already has a DefId in
// hand (so they go `DefId → DefInfo.ast_id`). Add a ptr_map only
// when a consumer needs the reverse direction.
struct AstIdMap {
    HashMap id_to_node;  // AstId.v (uint32_t) → struct Expr*
};

// Initialize an empty map. Storage comes from `s->arena`.
void ast_id_map_init(struct AstIdMap *map, struct Sema *s);

// Drop all entries. Called at the start of each (re)compute of
// query_top_level_index so the map describes the current revision
// only.
void ast_id_map_reset(struct AstIdMap *map);

// Insert an item, returning its assigned AstId. Computes the
// canonical hash from `(kind, name_id)` and probes forward on
// collision. Re-inserting the same logical item (same kind + name)
// at a different node pointer updates the node in place and returns
// the same AstId.
//
// The same parse calling sequence produces the same AstIds every
// time. Across reparses with stable item shape, AstIds are
// preserved. Across reparses where an item is added/removed/
// renamed, only the affected items' AstIds change.
AstId ast_id_map_insert(struct AstIdMap *map, DeclKind kind, StrId name_id,
                        struct Expr *node);

// Look up the AST node for an AstId. Returns NULL if the AstId
// doesn't correspond to a current entry (e.g., the item was removed
// in this revision).
struct Expr *ast_id_map_get(struct AstIdMap *map, AstId id);

#endif // ORE_SEMA_AST_ID_MAP_H
