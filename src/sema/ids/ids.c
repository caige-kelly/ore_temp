#include "ids.h"

#include <stddef.h>

#include "../../common/hashmap.h"
#include "../../common/vec.h"
#include "../../parser/ast.h"
#include "../modules/ast_id_map.h"
#include "../modules/def_map.h"
#include "../modules/modules.h"
#include "../scope/scope.h"
#include "../sema.h"

// Table representation: each ID family owns a Vec<void*> on Sema.
// Storing pointers (rather than embedding info structs by value) lets
// each layer define its own info struct in its own header without
// ids.c needing the full definition. The cost is one extra
// indirection per lookup; the benefit is layer independence.

void sema_ids_init(struct Sema *s) {
  if (s->defs_table)
    return;

  s->defs_table = vec_new_in(&s->arena, sizeof(void *));
  s->scopes_table = vec_new_in(&s->arena, sizeof(void *));
  s->modules_table = vec_new_in(&s->arena, sizeof(void *));
  s->bodies_table = vec_new_in(&s->arena, sizeof(void *));

  void *placeholder = NULL;
  vec_push(s->defs_table, &placeholder);
  vec_push(s->scopes_table, &placeholder);
  vec_push(s->modules_table, &placeholder);
  vec_push(s->bodies_table, &placeholder);
}

static uint32_t intern_into(Vec *tab, void *info) {
  vec_push(tab, &info);
  return (uint32_t)(tab->count - 1);
}

DefId sema_intern_def(struct Sema *s, struct DefInfo *info) {
  return (DefId){intern_into(s->defs_table, info)};
}

ScopeId sema_intern_scope(struct Sema *s, struct ScopeInfo *info) {
  return (ScopeId){intern_into(s->scopes_table, info)};
}

ModuleId sema_intern_module(struct Sema *s, struct ModuleInfo *info) {
  return (ModuleId){intern_into(s->modules_table, info)};
}

BodyId sema_intern_body(struct Sema *s, struct BodyInfo *info) {
  return (BodyId){intern_into(s->bodies_table, info)};
}

static void *lookup(Vec *tab, uint32_t idx) {
  if (!tab || idx == 0 || idx >= tab->count)
    return NULL;
  void **slot = (void **)vec_get(tab, idx);
  return slot ? *slot : NULL;
}

struct DefInfo *def_info(struct Sema *s, DefId id) {
  return (struct DefInfo *)lookup(s->defs_table, id.idx);
}

struct ScopeInfo *scope_info(struct Sema *s, ScopeId id) {
  return (struct ScopeInfo *)lookup(s->scopes_table, id.idx);
}

struct ModuleInfo *module_info(struct Sema *s, ModuleId id) {
  return (struct ModuleInfo *)lookup(s->modules_table, id.idx);
}

// Walk up the scope chain to find the owning module.
static ModuleId def_owning_module(struct Sema *s, struct DefInfo *di) {
  if (!di)
    return MODULE_ID_INVALID;
  ScopeId cur = di->owner_scope;
  while (scope_id_is_valid(cur)) {
    struct ScopeInfo *si = scope_info(s, cur);
    if (!si)
      break;
    if (module_id_is_valid(si->owner_module))
      return si->owner_module;
    cur = si->parent;
  }
  return MODULE_ID_INVALID;
}

// Resolve a def to its AST origin. Two paths:
//
// (1) Top-level DECL_USER / DECL_IMPORT (`di->ast_id` set): look up
//     in the owning module's AstIdMap. AstIds are stable across
//     reparses for items with unchanged (kind, name), so the
//     lookup picks up the current revision's Bind node regardless
//     of where it has shifted in the file. Mirrors rust-analyzer's
//     `AstId::to_node(db)` path.
//
// (2) Local DECL_USER (let-binds inside fn bodies / blocks),
//     DECL_PARAM, etc. (`di->origin_id` set): node_to_expr lookup.
//     Reliable because these DefInfo records are themselves rebuilt
//     by `scope_index_build_module` on every revision, so the
//     stored NodeId is always fresh.
//
// Other kinds (DECL_FIELD, DECL_VARIANT, DECL_PRIMITIVE, ...):
// origins aren't represented as Bind exprs and consumers don't ask.
// Return NULL.
struct Expr *def_origin(struct Sema *s, DefId id) {
  if (!s)
    return NULL;
  struct DefInfo *di = def_info(s, id);
  if (!di)
    return NULL;

  // Top-level binds: look up via AstIdMap (per-module hash table).
  if (ast_id_is_valid(di->ast_id)) {
    ModuleId mid = def_owning_module(s, di);
    if (!module_id_is_valid(mid))
      return NULL;
    struct ModuleInfo *m = module_info(s, mid);
    if (!m)
      return NULL;
    return ast_id_map_get(&m->ast_id_map, di->ast_id);
  }

  // Local / nested defs: rely on origin_id. These DefInfo records
  // are rebuilt each revision by scope_index, so origin_id is always
  // fresh and the node_to_expr lookup never goes stale.
  if (di->origin_id.id != 0 && s->node_to_expr.entries != NULL &&
      hashmap_contains(&s->node_to_expr, (uint64_t)di->origin_id.id))
    return (struct Expr *)hashmap_get(&s->node_to_expr,
                                      (uint64_t)di->origin_id.id);
  return NULL;
}

struct BodyInfo *body_info(struct Sema *s, BodyId id) {
  return (struct BodyInfo *)lookup(s->bodies_table, id.idx);
}

uint32_t sema_def_count(struct Sema *s) {
  return s->defs_table ? (uint32_t)s->defs_table->count : 0;
}

uint32_t sema_scope_count(struct Sema *s) {
  return s->scopes_table ? (uint32_t)s->scopes_table->count : 0;
}

uint32_t sema_module_count(struct Sema *s) {
  return s->modules_table ? (uint32_t)s->modules_table->count : 0;
}

uint32_t sema_body_count(struct Sema *s) {
  return s->bodies_table ? (uint32_t)s->bodies_table->count : 0;
}
