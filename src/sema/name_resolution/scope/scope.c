#include "scope.h"

#include <stddef.h>

#include "../../../support/common/arena.h"
#include "../../../db/ids/ids.h"
#include "../../workspace/def_map.h"
#include "../../../db/db.h"
#include "../../typechecker/decl_data.h"

Namespace ns_for_semantic(SemanticKind sem) {
  switch (sem) {
  case SEM_TYPE:
    return NS_TYPE;
  case SEM_EFFECT:
    return NS_EFFECT;
  case SEM_SCOPE_TOKEN: /* fallthrough */
  case SEM_EFFECT_ROW:
    return NS_OP;
  case SEM_UNKNOWN:
  case SEM_VALUE:
  case SEM_MODULE:
  default:
    return NS_VALUE;
  }
}

bool visibility_allows_external(Visibility vis) {
  return vis == Visibility_public;
}

ScopeId scope_create(struct Sema *s, ScopeKind kind, ScopeId parent,
                     ModuleId owner_module) {
  struct ScopeInfo *info = arena_alloc(&s->arena, sizeof(struct ScopeInfo));
  *info = (struct ScopeInfo){
      .kind = kind,
      .parent = parent,
      .owner_module = owner_module,
      .defs = vec_new_in(&s->arena, sizeof(DefId)),
      .children = vec_new_in(&s->arena, sizeof(ScopeId)),
  };
  hashmap_init_in(&info->name_index, &s->arena);

  ScopeId id = sema_intern_scope(s, info);

  if (scope_id_is_valid(parent)) {
    struct ScopeInfo *p = scope_info(s, parent);
    if (p)
      vec_push(p->children, &id);
  }
  return id;
}

DefId def_create(struct Sema *s, struct DefInfo proto) {
  struct DefInfo *info = arena_alloc(&s->arena, sizeof(struct DefInfo));
  *info = proto;
  return sema_intern_def(s, info);
}

// Common bookkeeping: append to defs vec + name_index. Returns false
// on duplicate name. The caller decides whether to update owner_scope.
static bool scope_insert_internal(struct Sema *s, ScopeId scope, DefId def,
                                  struct ScopeInfo **out_si,
                                  struct DefInfo **out_di) {
  struct ScopeInfo *si = scope_info(s, scope);
  struct DefInfo *di = def_info(s, def);
  if (!si || !di)
    return false;

  uint64_t name_key = (uint64_t)di->name_id.v;
  if (hashmap_contains(&si->name_index, name_key))
    return false;

  vec_push(si->defs, &def);
  // Pack DefId.idx into the void* slot. Using uintptr_t keeps the
  // round-trip lossless on 64-bit; we never dereference this as a
  // pointer.
  hashmap_put(&si->name_index, name_key, (void *)(uintptr_t)def.idx);

  if (out_si)
    *out_si = si;
  if (out_di)
    *out_di = di;
  return true;
}

bool scope_define_def(struct Sema *s, ScopeId scope, DefId def) {
  struct DefInfo *di = NULL;
  if (!scope_insert_internal(s, scope, def, NULL, &di))
    return false;
  // Canonical home: stamp ownership.
  di->owner_scope = scope;
  return true;
}

bool scope_mirror_def(struct Sema *s, ScopeId scope, DefId def) {
  // Same insertion, but no ownership change. The def's canonical
  // owner is wherever it was first defined.
  return scope_insert_internal(s, scope, def, NULL, NULL);
}

DefId scope_lookup_local(struct Sema *s, ScopeId scope, StrId name_id) {
  struct ScopeInfo *si = scope_info(s, scope);
  if (!si)
    return DEF_ID_INVALID;

  if (!hashmap_contains(&si->name_index, (uint64_t)name_id.v))
    return DEF_ID_INVALID;

  void *slot = hashmap_get(&si->name_index, (uint64_t)name_id.v);
  return (DefId){(uint32_t)(uintptr_t)slot};
}

// === Per-def AST-derived accessors ===

Visibility def_visibility(struct Sema *s, DefId def) {
  struct DefInfo *di = def_info(s, def);
  if (!di)
    return Visibility_private;
  switch (di->kind) {
  case DECL_USER:
  case DECL_IMPORT: {
    // Read pub from the current AST via the top-level index.
    struct Expr *origin = def_origin(s, def);
    if (origin && origin->kind == expr_Bind)
      return origin->bind.visibility;
    return Visibility_private;
  }
  case DECL_FIELD: {
    struct FieldLocator *loc = field_locator_get(s, def);
    if (!loc)
      return Visibility_private;
    struct StructSignature *sig = query_struct_signature(s, loc->parent_struct);
    if (sig && loc->index < sig->field_count)
      return sig->fields[loc->index].vis;
    return Visibility_private;
  }
  case DECL_VARIANT:
    return Visibility_public;
  case DECL_PRIMITIVE:
    return Visibility_public;
  default:
    return Visibility_private;
  }
}

struct Span def_span(struct Sema *s, DefId def) {
  struct DefInfo *di = def_info(s, def);
  if (!di)
    return (struct Span){0};
  switch (di->kind) {
  case DECL_USER:
  case DECL_IMPORT: {
    struct Expr *origin = def_origin(s, def);
    if (origin && origin->kind == expr_Bind)
      return origin->bind.name.span;
    return origin ? origin->span : (struct Span){0};
  }
  case DECL_FIELD: {
    struct FieldLocator *loc = field_locator_get(s, def);
    if (!loc)
      return (struct Span){0};
    struct StructSignature *sig = query_struct_signature(s, loc->parent_struct);
    if (sig && loc->index < sig->field_count)
      return sig->fields[loc->index].span;
    return (struct Span){0};
  }
  case DECL_VARIANT: {
    struct VariantLocator *loc = variant_locator_get(s, def);
    if (!loc)
      return (struct Span){0};
    struct EnumSignature *sig = query_enum_signature(s, loc->parent_enum);
    if (sig && loc->index < sig->variant_count)
      return sig->variants[loc->index].span;
    return (struct Span){0};
  }
  default:
    return (struct Span){0};
  }
}

SemanticKind def_semantic_kind(struct Sema *s, DefId def) {
  struct DefInfo *di = def_info(s, def);
  if (!di)
    return SEM_UNKNOWN;
  switch (di->kind) {
  case DECL_USER: {
    struct Expr *origin = def_origin(s, def);
    if (origin && origin->kind == expr_Bind)
      return sem_for_bind_value(origin->bind.value);
    return SEM_VALUE;
  }
  case DECL_IMPORT:
    return SEM_MODULE;
  case DECL_PRIMITIVE:
    return SEM_TYPE;
  case DECL_FIELD:
  case DECL_VARIANT:
  case DECL_PARAM:
  case DECL_LOOP_LABEL:
    return SEM_VALUE;
  case DECL_EFFECT_ROW:
    return SEM_EFFECT_ROW;
  case DECL_SCOPE_PARAM:
    return SEM_SCOPE_TOKEN;
  }
  return SEM_UNKNOWN;
}
