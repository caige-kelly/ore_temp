#include "def_map.h"

#include <stddef.h>

#include "support/data_structure/arena.h"
#include "support/data_structure/hashmap.h"
#include "parser/ast.h"
#include "db/query/query_engine.h"
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
static Fingerprint def_for_name_fp(DefId def, SemanticKind sem, Visibility vis,
                                   enum BindKind bind_kind) {
  Fingerprint fp = query_fingerprint_from_u64((uint64_t)def.idx);
  fp = query_fingerprint_combine(fp, query_fingerprint_from_u64((uint64_t)sem));
  fp = query_fingerprint_combine(fp, query_fingerprint_from_u64((uint64_t)vis));
  fp = query_fingerprint_combine(
      fp, query_fingerprint_from_u64((uint64_t)bind_kind));
  return fp;
}

SemanticKind sem_for_bind_value(struct Expr *value) {
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
  SEMA_QUERY_GUARD(s, &m->top_level_query, QUERY_TOP_LEVEL_INDEX, m, frame_span,
                   /*on_cached=*/m->top_level_index,
                   /*on_cycle=*/NULL,
                   /*on_error=*/NULL);

  Vec *ast = query_module_ast(s, mid);
  if (!ast) {
    // No AST means no top-level expressions. Two reasons this fires:
    //   1. Primitives module — no source; primitives_init populates
    //      scopes directly, so the top-level expression index is
    //      legitimately empty.
    //   2. Parse failure — query_module_ast itself is in ERROR
    //      state, and any consumer transitively depends on it
    //      through us, so the cascade still triggers when source
    //      becomes parseable again.
    // In neither case is "the top-level index is empty" an error
    // condition — it's a successful empty result, cached as a
    // None-valued memo (Salsa's Memo<Option<Output>> convention).
    // See bug_of_bugs.md B20.
    m->top_level_index = NULL;
    query_slot_set_fingerprint(&m->top_level_query,
                               query_fingerprint_from_u64(0));
    sema_query_succeed(s, &m->top_level_query);
    return NULL;
  }

  Vec *idx = vec_new_in(&s->arena, sizeof(struct TopLevelEntry));
  // Rebuild the AstIdMap alongside the top-level index. Reset first
  // so prior revisions' (Expr*) entries — now stale — don't linger.
  ast_id_map_reset(&m->ast_id_map);
  for (size_t i = 0; i < ast->count; i++) {
    struct Expr **slot = (struct Expr **)vec_get(ast, i);
    struct Expr *e = slot ? *slot : NULL;
    if (!e)
      continue;

    switch (e->kind) {
    case expr_Bind: {
      // Assign a stable AstId for this top-level item. The hash is
      // (DECL_USER, name) — modules don't have nested items today,
      // so parent_id = 0 implicitly. Same (kind, name) → same AstId
      // every reparse, regardless of byte position. Insert into the
      // map first, then stash the assigned id on the entry so
      // query_def_for_name can copy it to DefInfo without re-running
      // the hash + probe walk.
      AstId aid = ast_id_map_insert(&m->ast_id_map, DECL_USER,
                                    e->bind.name.string_id, e);
      struct TopLevelEntry entry = {
          .name_id = e->bind.name.string_id,
          .node = e,
          .vis = e->bind.visibility,
          .span = e->bind.name.span,
          .is_destructure = false,
          .ast_id = aid,
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
      // — needs walker." No AstId is allocated until the pattern
      // walker assigns names to leaves.
      struct TopLevelEntry entry = {
          .name_id = STR_ID_NONE,
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
    fp =
        query_fingerprint_combine(fp, query_fingerprint_from_u64(e->name_id.v));
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
get_or_create_entry(struct Sema *s, struct ModuleInfo *m, StrId name_id) {
  uint64_t key = (uint64_t)name_id.v;
  if (hashmap_contains(&m->def_map_entries, key))
    return (struct DefMapEntry *)hashmap_get(&m->def_map_entries, key);

  struct DefMapEntry *entry =
      arena_alloc(&s->arena, sizeof(struct DefMapEntry));
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
struct TopLevelEntry *find_top_level(Vec *idx, StrId name_id) {
  if (!idx || name_id.v == 0)
    return NULL;
  for (size_t i = 0; i < idx->count; i++) {
    struct TopLevelEntry *e = (struct TopLevelEntry *)vec_get(idx, i);
    if (e && e->name_id.v == name_id.v)
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
  m->export_scope =
      scope_create(s, m->is_primitives ? SCOPE_PRIMITIVES : SCOPE_MODULE,
                   SCOPE_ID_INVALID, mid);
}

DefId query_def_for_name(struct Sema *s, ModuleId mid, StrId name_id) {
  struct ModuleInfo *m = module_info(s, mid);
  if (!m || name_id.v == 0)
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
  // those caches AND collide with the existing scope name.
  //
  // We do NOT refresh any DefInfo fields here. DefInfo is identity
  // only (kind + name + scope position); span / vis / origin /
  // semantic_kind are derived on demand via the per-def accessors
  // (def_span / def_visibility / def_origin / def_semantic_kind),
  // which read the current AST through query_top_level_index. This
  // matches rust-analyzer: `FunctionLoc` is immutable; AST-derived
  // data lives in tracked queries. Before this refactor, the refresh
  // here was gated by SEMA_QUERY_GUARD's cached path and silently
  // skipped on revisions where def_for_name's *output* was unchanged
  // even when the AST had shifted — root cause of the LSP stale-
  // origin class of bug.
  SemanticKind sem = sem_for_bind_value(b->value);
  DefId def = entry->def;
  if (!def_id_is_valid(def)) {
    // First time this name is queried in the module — allocate the
    // DefId and stitch it into the module's scopes.
    //
    // `ast_id` is the stable handle into the module's AstIdMap (set
    // by query_top_level_index when this entry was emitted). The
    // origin_id slot is reserved for local defs allocated by
    // scope_index, which need a per-parse NodeId; top-level defs
    // use ast_id and leave origin_id zero. `def_origin` dispatches
    // on which one is set.
    struct DefInfo proto = {
        .kind = DECL_USER,
        .name_id = b->name.string_id,
        .ast_id = src->ast_id,
        .origin_id = (struct NodeId){0},
        .owner_scope = m->internal_scope,
    };
    def = def_create(s, proto);

    // R8: `node_to_expr` was deleted. Top-level def_origin uses the
    // module's AstIdMap directly (populated by query_top_level_index),
    // so no eager-population bridge is needed here.

    if (!scope_define_def(s, m->internal_scope, def)) {
      // Duplicate top-level name. Diagnostic emission lives with
      // diag/codes.h (E0100 namespace family); treat as failure.
      sema_query_fail(s, &entry->query);
      return DEF_ID_INVALID;
    }
    if (b->visibility == Visibility_public)
      scope_mirror_def(s, m->export_scope, def);

    entry->def = def;
  }

  // Common tail: stamp the slot's fingerprint over the externally
  // observable shape (def + sem + vis + bind_kind). Same code path
  // for first-time allocation and revalidation — a vis/sem flip
  // produces a different fingerprint, propagating through consumers'
  // dep tracking.
  query_slot_set_fingerprint(&entry->query,
                             def_for_name_fp(def, sem, b->visibility, b->kind));
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
    if (!e || e->name_id.v == 0)
      continue;
    if (!def_id_is_valid(query_def_for_name(s, mid, e->name_id)))
      ok = false;
  }
  return ok;
}
