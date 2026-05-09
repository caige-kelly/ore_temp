#include "def_map.h"

#include <stddef.h>

#include "../../common/arena.h"
#include "../../common/hashmap.h"
#include "../../parser/ast.h"
#include "../query/query_engine.h"
#include "../resolve/scope_index.h"
#include "../scope/scope.h"
#include "../sema.h"
#include "modules.h"

// SemanticKind classification from the Bind's RHS shape. Defaults to
// SEM_VALUE — functions, constants, and "we don't know yet" all map
// to value-namespace lookups. Type/effect classification is purely
// syntactic at the def-map level; deeper checks (e.g. that an
// expr_Lambda actually returns a value) happen at typecheck.
// Fingerprint over the externally observable shape of a `def_for_name`
// result: WHO the name binds to (DefId), WHAT semantic flavour it
// resolves at (value/type/effect), WHETHER cross-module consumers can
// see it (vis), and the BIND SHAPE (`::` vs. `:=` vs. typed). A
// body-only edit to the bind's RHS leaves all four unchanged, so
// dependents that only care about the slot's identity early-cut. A
// `pub` toggle, rename to a different DECL_USER, or `::` → `:=` flips
// the fingerprint and consumers revalidate. (Without this, the slot's
// fingerprint stayed at FINGERPRINT_NONE and PR 1 had to depend on
// query_module_ast directly to compensate. See B9.)
static Fingerprint def_for_name_fp(DefId def, SemanticKind sem,
                                   Visibility vis, enum BindKind bind_kind) {
  Fingerprint fp = query_fingerprint_from_u64((uint64_t)def.idx);
  fp = query_fingerprint_combine(fp,
                                 query_fingerprint_from_u64((uint64_t)sem));
  fp = query_fingerprint_combine(fp,
                                 query_fingerprint_from_u64((uint64_t)vis));
  fp = query_fingerprint_combine(
      fp, query_fingerprint_from_u64((uint64_t)bind_kind));
  return fp;
}

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
// Single AST scan, no DefId allocation. The slot lives on ModuleInfo
// (`top_level_query`). Calling `query_module_ast` from inside the
// guarded body records a dep on that producer slot — the invalidation
// walker will mark this slot dirty when the AST re-parses, which is
// what guarantees `m->top_level_index` doesn't outlive its source AST.

Vec *query_top_level_index(struct Sema *s, ModuleId mid) {
  struct ModuleInfo *m = module_info(s, mid);
  if (!m)
    return NULL;

  struct Span frame_span = {0};
  SEMA_QUERY_GUARD(s, &m->top_level_query, QUERY_TOP_LEVEL_INDEX, m,
                   frame_span,
                   /*on_cached=*/m->top_level_index,
                   /*on_cycle=*/NULL,
                   /*on_error=*/NULL);

  Vec *ast = query_module_ast(s, mid);
  if (!ast) {
    sema_query_fail(s, &m->top_level_query);
    return NULL;
  }

  Vec *idx = vec_new_in(&s->arena, sizeof(struct TopLevelEntry));
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
          .vis = e->destructure.is_pub ? Visibility_public : Visibility_private,
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

  // Fingerprint the entries — name_id + visibility + is_destructure.
  // NOT the Expr* pointers (those flip every re-parse) and NOT spans
  // (line shifts shouldn't invalidate downstream resolve queries).
  Fingerprint fp = query_fingerprint_from_u64(idx->count);
  for (size_t i = 0; i < idx->count; i++) {
    struct TopLevelEntry *e = (struct TopLevelEntry *)vec_get(idx, i);
    fp = query_fingerprint_combine(fp, query_fingerprint_from_u64(e->name_id));
    uint64_t flags = ((uint64_t)e->vis << 1) | (e->is_destructure ? 1 : 0);
    fp = query_fingerprint_combine(fp, query_fingerprint_from_u64(flags));
  }
  query_slot_set_fingerprint(&m->top_level_query, fp);

  sema_query_succeed(s, &m->top_level_query);
  return idx;
}

// === query_def_for_name ===
//
// Per-name lazy DefId construction. Allocates exactly once per name;
// subsequent calls hit the cached entry's slot (DONE state).

static struct DefMapEntry *
get_or_create_entry(struct Sema *s, struct ModuleInfo *m, uint32_t name_id) {
  uint64_t key = (uint64_t)name_id;
  if (hashmap_contains(&m->def_map_entries, key))
    return (struct DefMapEntry *)hashmap_get(&m->def_map_entries, key);

  struct DefMapEntry *entry = arena_alloc(&s->arena, sizeof(struct DefMapEntry));
  *entry = (struct DefMapEntry){
      .name_id = name_id,
      .def = DEF_ID_INVALID,
  };
  sema_query_slot_init(&entry->query, QUERY_DEF_FOR_NAME);
  hashmap_put_or_die(&m->def_map_entries, key, entry, "def_map_entries");
  return entry;
}

// Look up `name_id` in the module's top-level index. Returns NULL
// when the name isn't a top-level entry — caller handles by
// returning DEF_ID_INVALID.
static struct TopLevelEntry *find_top_level(Vec *idx, uint32_t name_id) {
  if (!idx || name_id == 0)
    return NULL;
  for (size_t i = 0; i < idx->count; i++) {
    struct TopLevelEntry *e = (struct TopLevelEntry *)vec_get(idx, i);
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
  if (!m->is_primitives && module_id_is_valid(s->primitives_module)) {
    struct ModuleInfo *primitives = module_info(s, s->primitives_module);
    if (primitives)
      parent = primitives->export_scope;
  }
  m->internal_scope = scope_create(
      s, m->is_primitives ? SCOPE_PRIMITIVES : SCOPE_MODULE, parent, mid);
  m->export_scope = scope_create(
      s, m->is_primitives ? SCOPE_PRIMITIVES : SCOPE_MODULE, SCOPE_ID_INVALID, mid);
}

DefId query_def_for_name(struct Sema *s, ModuleId mid, uint32_t name_id) {
  struct ModuleInfo *m = module_info(s, mid);
  if (!m || name_id == 0)
    return DEF_ID_INVALID;

  // Lazy hashmap init — Vec/HashMap fields default to zero in
  // module_create, hashmap_init_in lives here on first access.
  if (m->def_map_entries.entries == NULL)
    hashmap_init_in(&m->def_map_entries, &s->arena);

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

  // If a DefId was already allocated for this name on a previous
  // pass (the entry's slot was DONE and is now recomputing after a
  // dep changed), reuse it. The DefId lives in the module's scope
  // and is referenced by downstream caches keyed by DefId
  // (SemaDeclInfo, FnSignature, etc.) — reallocating would orphan
  // those caches AND collide with the existing scope name. Refresh
  // the DefInfo's AST-pointing fields (origin, origin_id, span)
  // since the post-reparse AST nodes are fresh allocations even
  // when structurally identical.
  //
  // The semantic_kind / visibility fields might have changed in the
  // edit (e.g., user toggled `pub`), so refresh those too.
  if (def_id_is_valid(entry->def)) {
    struct DefInfo *di = def_info(s, entry->def);
    if (di) {
      di->semantic_kind = sem_for_bind_value(b->value);
      di->span          = b->name.span;
      di->origin_id     = src->node->id;
      di->origin        = src->node;
      di->vis           = b->visibility;
      // child_scope is reset by signature queries when they re-run;
      // imported_module / scope_token_id stay (kind-specific, set
      // below for first-time creates).
    }
    query_slot_set_fingerprint(
        &entry->query,
        def_for_name_fp(entry->def, sem_for_bind_value(b->value),
                        b->visibility, b->kind));
    sema_query_succeed(s, &entry->query);
    return entry->def;
  }

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
  };
  DefId def = def_create(s, proto);

  // Eagerly populate node_to_expr for the top-level Bind so
  // `def_origin(s, def)` can resolve its AST node via NodeId without
  // waiting for the per-fn scope walk. Critical for queries that read
  // origins between def_map and scope_index (e.g., struct/enum
  // signature queries triggered from typecheck).
  scope_index_record_node(s, src->node);

  if (!scope_define_def(s, m->internal_scope, def)) {
    // Duplicate top-level name. Diagnostic emission lives with
    // diag/codes.h (E0100 namespace family); treat as failure.
    sema_query_fail(s, &entry->query);
    return DEF_ID_INVALID;
  }
  if (b->visibility == Visibility_public)
    scope_mirror_def(s, m->export_scope, def);

  entry->def = def;
  query_slot_set_fingerprint(
      &entry->query,
      def_for_name_fp(def, proto.semantic_kind, b->visibility, b->kind));
  sema_query_succeed(s, &entry->query);
  return def;
}

bool def_map_collect_top_level(struct Sema *s, ModuleId mid) {
  Vec *idx = query_top_level_index(s, mid);
  if (!idx)
    return true;

  bool ok = true;
  for (size_t i = 0; i < idx->count; i++) {
    struct TopLevelEntry *e = (struct TopLevelEntry *)vec_get(idx, i);
    if (!e || e->name_id == 0)
      continue;
    if (!def_id_is_valid(query_def_for_name(s, mid, e->name_id)))
      ok = false;
  }
  return ok;
}
