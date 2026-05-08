#include "def_map.h"

#include <stddef.h>

#include "../../common/arena.h"
#include "../../common/hashmap.h"
#include "../../parser/ast.h"
#include "../scope/scope.h"
#include "../sema.h"
#include "modules.h"

// SemanticKind classification from the Bind's RHS shape. Defaults to
// SEM_VALUE — functions, constants, and "we don't know yet" all map
// to value-namespace lookups. Type/effect classification is purely
// syntactic at the def-map level; deeper checks (e.g. that an
// expr_Lambda actually returns a value) happen at typecheck.
static SemanticKind sem_for_bind_value(struct Expr *value) {
  if (!value)
    return SEM_VALUE;
  switch (value->kind) {
  case expr_Struct:
  case expr_Enum:
    return SEM_TYPE;
  case decl_Effect:
    return SEM_EFFECT;
  default:
    return SEM_VALUE;
  }
}

// === query_top_level_index ===
//
// Single AST scan, no DefId allocation. Re-walks only on AST re-parse.

Vec *query_top_level_index(struct Sema *s, ModuleId mid) {
  struct ModuleInfo *m = module_info(s, mid);
  if (!m)
    return NULL;
  if (m->top_level_index)
    return m->top_level_index;

  Vec *ast = m->ast ? m->ast : query_module_ast(s, mid);
  if (!ast)
    return NULL;

  Vec *idx = vec_new_in(s->arena, sizeof(struct TopLevelEntry));
  for (size_t i = 0; i < ast->count; i++) {
    struct Expr **slot = (struct Expr **)vec_get(ast, i);
    struct Expr *e = slot ? *slot : NULL;
    if (!e)
      continue;

    switch (e->kind) {
    case expr_Bind: {
      struct TopLevelEntry entry = {
          .name_id = e->bind.name.string_id,
          .node = e,
          .vis = e->bind.visibility,
          .span = e->bind.name.span,
          .is_destructure = false,
      };
      vec_push(idx, &entry);
      break;
    }
    case expr_DestructureBind: {
      // A destructure-bind contributes multiple names; we record one
      // entry per leaf in the pattern. Pattern-leaf walking is its
      // own concern that lands when the resolver actually exercises
      // destructure-bind paths. For now, record an entry with name
      // 0 so query_def_for_name can flag it as "destructure shape
      // — needs walker."
      struct TopLevelEntry entry = {
          .name_id = 0,
          .node = e,
          .vis = e->destructure.is_pub ? Visibility_public
                                       : Visibility_private,
          .span = e->span,
          .is_destructure = true,
      };
      vec_push(idx, &entry);
      break;
    }
    case decl_Effect:
    default:
      // Loose top-level expressions (calls, comptime blocks) and
      // bare effect-decl expressions don't introduce a top-level
      // name. Ignored by the index.
      break;
    }
  }

  m->top_level_index = idx;
  return idx;
}

// === query_def_for_name ===
//
// Per-name lazy DefId construction. Allocates exactly once per name;
// subsequent calls hit the cached entry's slot (DONE state).

static struct DefMapEntry *get_or_create_entry(struct Sema *s,
                                               struct ModuleInfo *m,
                                               uint32_t name_id) {
  uint64_t key = (uint64_t)name_id;
  if (hashmap_contains(&m->def_map_entries, key))
    return (struct DefMapEntry *)hashmap_get(&m->def_map_entries, key);

  struct DefMapEntry *entry =
      arena_alloc(s->arena, sizeof(struct DefMapEntry));
  *entry = (struct DefMapEntry){
      .name_id = name_id,
      .def = DEF_ID_INVALID,
  };
  sema_query_slot_init(&entry->query, QUERY_DEF_FOR_NAME);
  hashmap_put(&m->def_map_entries, key, entry);
  return entry;
}

// Look up `name_id` in the module's top-level index. Returns NULL
// when the name isn't a top-level entry — caller handles by
// returning DEF_ID_INVALID.
static struct TopLevelEntry *find_top_level(Vec *idx, uint32_t name_id) {
  if (!idx || name_id == 0)
    return NULL;
  for (size_t i = 0; i < idx->count; i++) {
    struct TopLevelEntry *e =
        (struct TopLevelEntry *)vec_get(idx, i);
    if (e && e->name_id == name_id)
      return e;
  }
  return NULL;
}

// Build the module-internal/export scopes lazily. Mirrors the eager
// construction in the previous query_module_def_map; we still need
// the scopes before the first def can be inserted.
static void ensure_module_scopes(struct Sema *s, struct ModuleInfo *m,
                                 ModuleId mid) {
  if (scope_id_is_valid(m->internal_scope))
    return;
  ScopeId parent = SCOPE_ID_INVALID;
  if (!m->is_prelude && module_id_is_valid(s->prelude_module)) {
    struct ModuleInfo *prelude = module_info(s, s->prelude_module);
    if (prelude)
      parent = prelude->export_scope;
  }
  m->internal_scope = scope_create(
      s, m->is_prelude ? SCOPE_PRELUDE : SCOPE_MODULE, parent, mid);
  m->export_scope = scope_create(
      s, m->is_prelude ? SCOPE_PRELUDE : SCOPE_MODULE, SCOPE_ID_INVALID, mid);
}

DefId query_def_for_name(struct Sema *s, ModuleId mid, uint32_t name_id) {
  struct ModuleInfo *m = module_info(s, mid);
  if (!m || name_id == 0)
    return DEF_ID_INVALID;

  // Lazy hashmap init — Vec/HashMap fields default to zero in
  // module_create, hashmap_init_in lives here on first access.
  if (m->def_map_entries.entries == NULL)
    hashmap_init_in(&m->def_map_entries, s->arena);

  struct DefMapEntry *entry = get_or_create_entry(s, m, name_id);

  struct Span frame_span = {0};
  SEMA_QUERY_GUARD(s, &entry->query, QUERY_DEF_FOR_NAME, entry, frame_span,
                   /*on_cached=*/entry->def,
                   /*on_cycle=*/DEF_ID_INVALID,
                   /*on_error=*/DEF_ID_INVALID);

  Vec *idx = query_top_level_index(s, mid);
  struct TopLevelEntry *src = find_top_level(idx, name_id);
  if (!src) {
    sema_query_fail(s, &entry->query);
    return DEF_ID_INVALID;
  }

  ensure_module_scopes(s, m, mid);

  // Destructure-binds need a pattern walker; defer until tests
  // exercise them. For the common single-name case, take the bind
  // shape and allocate a DefInfo.
  if (src->is_destructure) {
    sema_query_fail(s, &entry->query);
    return DEF_ID_INVALID;
  }

  struct BindExpr *b = &src->node->bind;
  struct DefInfo proto = {
      .kind = DECL_USER,
      .semantic_kind = sem_for_bind_value(b->value),
      .name_id = b->name.string_id,
      .span = b->name.span,
      .origin_id = src->node->id,
      .origin = src->node,
      .owner_scope = m->internal_scope,
      .child_scope = SCOPE_ID_INVALID,
      .imported_module = MODULE_ID_INVALID,
      .vis = b->visibility,
      .scope_token_id = 0,
      .is_comptime = src->node->is_comptime,
      .has_effects = false,
  };
  DefId def = def_create(s, proto);
  if (!scope_insert_def(s, m->internal_scope, def)) {
    // Duplicate top-level name. Diagnostic emission lives with
    // diag/codes.h (E0100 namespace family); treat as failure.
    sema_query_fail(s, &entry->query);
    return DEF_ID_INVALID;
  }
  if (b->visibility == Visibility_public)
    scope_insert_def(s, m->export_scope, def);

  entry->def = def;
  sema_query_succeed(s, &entry->query);
  return def;
}

bool def_map_collect_top_level(struct Sema *s, ModuleId mid) {
  Vec *idx = query_top_level_index(s, mid);
  if (!idx)
    return true;

  bool ok = true;
  for (size_t i = 0; i < idx->count; i++) {
    struct TopLevelEntry *e =
        (struct TopLevelEntry *)vec_get(idx, i);
    if (!e || e->name_id == 0)
      continue;
    if (!def_id_is_valid(query_def_for_name(s, mid, e->name_id)))
      ok = false;
  }
  return ok;
}
