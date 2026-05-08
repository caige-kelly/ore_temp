#include "scope.h"

#include <stddef.h>

#include "../../common/arena.h"
#include "../sema.h"

Namespace ns_for_semantic(SemanticKind sem) {
  switch (sem) {
  case SEM_TYPE:        return NS_TYPE;
  case SEM_EFFECT:      return NS_EFFECT;
  case SEM_SCOPE_TOKEN: /* fallthrough */
  case SEM_EFFECT_ROW:  return NS_OP;
  case SEM_UNKNOWN:
  case SEM_VALUE:
  case SEM_MODULE:
  default:              return NS_VALUE;
  }
}

bool visibility_allows_external(Visibility vis) {
  return vis == Visibility_public;
}

ScopeId scope_create(struct Sema *s, ScopeKind kind, ScopeId parent,
                     ModuleId owner_module) {
  struct ScopeInfo *info = arena_alloc(s->arena, sizeof(struct ScopeInfo));
  *info = (struct ScopeInfo){
      .kind = kind,
      .parent = parent,
      .owner_module = owner_module,
      .defs = vec_new_in(s->arena, sizeof(DefId)),
      .children = vec_new_in(s->arena, sizeof(ScopeId)),
  };
  hashmap_init_in(&info->name_index, s->arena);

  ScopeId id = sema_intern_scope(s, info);

  if (scope_id_is_valid(parent)) {
    struct ScopeInfo *p = scope_info(s, parent);
    if (p)
      vec_push(p->children, &id);
  }
  return id;
}

DefId def_create(struct Sema *s, struct DefInfo proto) {
  struct DefInfo *info = arena_alloc(s->arena, sizeof(struct DefInfo));
  *info = proto;
  return sema_intern_def(s, info);
}

bool scope_insert_def(struct Sema *s, ScopeId scope, DefId def) {
  struct ScopeInfo *si = scope_info(s, scope);
  struct DefInfo *di = def_info(s, def);
  if (!si || !di)
    return false;

  uint64_t name_key = (uint64_t)di->name_id;
  if (hashmap_contains(&si->name_index, name_key))
    return false;

  vec_push(si->defs, &def);
  // Pack DefId.idx into the void* slot. Using uintptr_t keeps the
  // round-trip lossless on 64-bit; we never dereference this as a
  // pointer.
  hashmap_put(&si->name_index, name_key, (void *)(uintptr_t)def.idx);
  di->owner_scope = scope;
  return true;
}

DefId scope_lookup_local(struct Sema *s, ScopeId scope, uint32_t name_id) {
  struct ScopeInfo *si = scope_info(s, scope);
  if (!si)
    return DEF_ID_INVALID;

  if (!hashmap_contains(&si->name_index, (uint64_t)name_id))
    return DEF_ID_INVALID;

  void *slot = hashmap_get(&si->name_index, (uint64_t)name_id);
  return (DefId){(uint32_t)(uintptr_t)slot};
}
