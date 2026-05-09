#include "modules.h"

#include <stddef.h>

#include "../../common/arena.h"
#include "../../common/hashmap.h"
#include "../../parser/parse_source.h"
#include "../query/query_engine.h"
#include "../scope/scope.h"
#include "../sema.h"
#include "def_map.h"

ModuleId module_create(struct Sema *s, InputId input, bool is_primitives) {
  struct ModuleInfo *info = arena_alloc(&s->arena, sizeof(struct ModuleInfo));
  *info = (struct ModuleInfo){
      .input = input,
      .ast = NULL,
      .ast_fp = FINGERPRINT_NONE,
      .internal_scope = SCOPE_ID_INVALID,
      .export_scope = SCOPE_ID_INVALID,
      .imports = NULL,
      .is_primitives = is_primitives,
      .resolving = false,
      .resolved = false,
  };
  sema_query_slot_init(&info->def_map_query, QUERY_MODULE_DEF_MAP);
  sema_query_slot_init(&info->exports_query, QUERY_MODULE_EXPORTS);
  sema_query_slot_init(&info->top_level_query, QUERY_TOP_LEVEL_INDEX);

  ModuleId id = sema_intern_module(s, info);

  // Cache by path so query_module_for_path can dedupe. The path_id
  // lives on the InputInfo; pull it out for the lookup table.
  if (input_id_is_valid(input)) {
    struct InputInfo *ii = input_info(s, input);
    if (ii && ii->path_id != 0)
      hashmap_put(&s->module_by_path, (uint64_t)ii->path_id,
                  (void *)(uintptr_t)id.idx);
  }

  return id;
}

Vec *query_module_ast(struct Sema *s, ModuleId mid) {
  struct ModuleInfo *m = module_info(s, mid);
  if (!m)
    return NULL;
  // The primitives module has no source; its def_map is built
  // directly by primitives_init without going through the parser.
  if (!input_id_is_valid(m->input))
    return NULL;

  struct InputInfo *ii = input_info(s, m->input);
  if (!ii)
    return NULL;

  // Slot key is the InputInfo* — distinct inputs get distinct slots.
  // Use the input's source span (zeroed for now since the AST is the
  // module's whole source — we don't have a meaningful single-token
  // span at the module level).
  struct Span frame_span = {0};
  SEMA_QUERY_GUARD(s, &ii->ast_query, QUERY_MODULE_AST, ii, frame_span,
                   /*on_cached=*/m->ast,
                   /*on_cycle=*/NULL,
                   /*on_error=*/NULL);

  const char *source = query_input_source(s, m->input);
  if (!source) {
    sema_query_fail(s, &ii->ast_query);
    return NULL;
  }

  // Use the input's idx as the lexer file_id for spans. Wiring spans
  // to the SourceMap for diagnostic display is a follow-up; today the
  // file_id is just an opaque token-stamp value.
  Vec *ast = parse_source(&s->arena, &s->arena, &s->pool, &s->diags,
                          (int)m->input.idx, source, ii->source_len);
  if (!ast) {
    sema_query_fail(s, &ii->ast_query);
    return NULL;
  }

  m->ast = ast;
  // AST fingerprint = source-byte hash. Any content change shifts
  // the fp; downstream queries that read AST data through a dep on
  // this slot will revalidate and recompute against the new tree.
  //
  // The previous design used a coarse structural hash over
  // (count, top-level kinds, top-level names) intended to early-cut
  // body-only edits. That early-cutoff is now achieved one level
  // deeper instead: per-decl signature/type queries fingerprint
  // their own structural output (param+ret types, struct fields,
  // etc.). When a body-only edit recomputes a signature query, the
  // signature's fingerprint is unchanged, so consumers of the
  // signature still skip recompute — same property, locally
  // enforced. The win: a signature edit that leaves top-level names
  // intact (like `f :: fn() -> i32` → `fn() -> u8`) now invalidates
  // signature queries whose Type result actually changed, instead
  // of being silently masked by a stale top-level structural hash.
  //
  // ii->source_fp was computed in sema_set_input_source over the
  // raw bytes; reuse it here so we don't double-hash.
  m->ast_fp = ii->source_fp;
  query_slot_set_fingerprint(&ii->ast_query, m->ast_fp);
  ii->is_dirty = false;

  sema_query_succeed(s, &ii->ast_query);
  return ast;
}

// Structural fingerprint for a module's top-level def map.
//
// Hashes over the externally observable shape of every top-level
// entry: `(name_id, visibility, semantic_kind)`. Notably ABSENT:
// spans, body contents, and the DefId itself. That selectivity is
// the load-bearing property — a body-internal edit (a comment in a
// fn, a different RHS for a const) reparses the AST and reruns
// def_map, but the fingerprint is unchanged, so downstream queries
// that depend on def_map (resolve_ref, future cross-module
// resolves) skip recompute via early cutoff.
//
// `include_private` selects between two views:
//   true  → full def map fp (used by query_module_def_map). Adding
//           or renaming any top-level decl, public OR private,
//           shifts the fp.
//   false → exports-only fp (used by query_module_exports). Edits
//           to private decls don't shift the fp, so importers stay
//           cached when only the private surface changes.
//
// Iterates `top_level_index` in source order — deterministic.
// Looks up each entry's resolved DefMapEntry (already populated by
// `def_map_collect_top_level`) to recover the semantic_kind.
static Fingerprint compute_def_map_fp(struct Sema *s, struct ModuleInfo *m,
                                      bool include_private) {
  Fingerprint fp = query_fingerprint_from_u64(0);
  Vec *idx = m->top_level_index;
  if (!idx)
    return fp;

  // Salt with the live entry count so empty-vs-nonempty are distinct,
  // and so adding/removing entries always shifts the fp regardless of
  // their fields.
  size_t live_count = 0;
  for (size_t i = 0; i < idx->count; i++) {
    struct TopLevelEntry *e = (struct TopLevelEntry *)vec_get(idx, i);
    if (!e || e->name_id == 0) continue;
    if (!include_private && e->vis != Visibility_public) continue;
    live_count++;
  }
  fp = query_fingerprint_combine(fp, query_fingerprint_from_u64(live_count));

  for (size_t i = 0; i < idx->count; i++) {
    struct TopLevelEntry *e = (struct TopLevelEntry *)vec_get(idx, i);
    if (!e || e->name_id == 0) continue;
    if (!include_private && e->vis != Visibility_public) continue;

    SemanticKind sem = SEM_UNKNOWN;
    if (hashmap_contains(&m->def_map_entries, (uint64_t)e->name_id)) {
      struct DefMapEntry *dme = (struct DefMapEntry *)hashmap_get(
          &m->def_map_entries, (uint64_t)e->name_id);
      if (dme) {
        struct DefInfo *di = def_info(s, dme->def);
        if (di) sem = di->semantic_kind;
      }
    }

    // Pack (name_id, vis, sem) into one u64. name_id is 32 bits;
    // vis is one byte; sem is one byte. Plenty of headroom.
    uint64_t packed = ((uint64_t)e->name_id) |
                      ((uint64_t)e->vis << 32) |
                      ((uint64_t)sem << 40);
    fp = query_fingerprint_combine(fp, query_fingerprint_from_u64(packed));
  }
  return fp;
}

bool query_module_def_map(struct Sema *s, ModuleId mid) {
  struct ModuleInfo *m = module_info(s, mid);
  if (!m)
    return false;

  // Use a synthetic span for the query frame; real diagnostics use
  // each item's own span. The frame span is only consulted on cycles.
  struct Span frame_span = {0};
  SEMA_QUERY_GUARD(s, &m->def_map_query, QUERY_MODULE_DEF_MAP, m, frame_span,
                   /*on_cached=*/true, /*on_cycle=*/false, /*on_error=*/false);

  // Lazily create the scopes the moment def_map runs. User module
  // internal_scope parents to the primitives module's exports so
  // primitive type lookups (`u8`, `i32`, ...) succeed; the
  // primitives module itself has no parent.
  if (!scope_id_is_valid(m->internal_scope)) {
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

  m->resolving = true;
  bool ok = def_map_collect_top_level(s, mid);
  m->resolving = false;

  if (ok) {
    m->resolved = true;
    Fingerprint fp = compute_def_map_fp(s, m, /*include_private=*/true);
    query_slot_set_fingerprint(&m->def_map_query, fp);
    sema_query_succeed(s, &m->def_map_query);
  } else {
    sema_query_fail(s, &m->def_map_query);
  }
  return ok;
}

ScopeId query_module_exports(struct Sema *s, ModuleId mid) {
  struct ModuleInfo *m = module_info(s, mid);
  if (!m)
    return SCOPE_ID_INVALID;

  struct Span frame_span = {0};
  SEMA_QUERY_GUARD(s, &m->exports_query, QUERY_MODULE_EXPORTS, m, frame_span,
                   /*on_cached=*/m->export_scope,
                   /*on_cycle=*/SCOPE_ID_INVALID,
                   /*on_error=*/SCOPE_ID_INVALID);

  // Exports depend on def_map having resolved every top-level entry —
  // calling def_map records the dep and ensures we have a populated
  // top_level_index + def_map_entries for the public-subset hash.
  if (!query_module_def_map(s, mid)) {
    sema_query_fail(s, &m->exports_query);
    return SCOPE_ID_INVALID;
  }

  // Public-only fingerprint. Importers depend on this query, so
  // they're stable across edits to private decls.
  Fingerprint fp = compute_def_map_fp(s, m, /*include_private=*/false);
  query_slot_set_fingerprint(&m->exports_query, fp);
  sema_query_succeed(s, &m->exports_query);
  return m->export_scope;
}

ModuleId query_module_for_path(struct Sema *s, uint32_t path_id,
                               struct Span span) {
  (void)span;
  if (path_id == 0)
    return MODULE_ID_INVALID;

  // Cached?
  if (hashmap_contains(&s->module_by_path, (uint64_t)path_id)) {
    void *slot = hashmap_get(&s->module_by_path, (uint64_t)path_id);
    return (ModuleId){(uint32_t)(uintptr_t)slot};
  }

  // Cross-file loading is deferred — when we tackle multi-file,
  // this is where parse-on-demand plugs in:
  //   1. Resolve path_id to canonical filesystem path.
  //   2. Lex + parse the file (calling into parser.h).
  //   3. module_create with the resulting AST.
  //   4. The new ModuleId is auto-cached in module_by_path.
  // For now, single-file programs don't trigger this branch.
  return MODULE_ID_INVALID;
}

// span.file_id corresponds to an InputId.idx. Walk modules_table to
// find the module that owns that input. Linear scan; modules_table
// holds at most a handful of entries in any realistic program.
ModuleId module_for_span(struct Sema *s, struct Span span) {
  if (!s || span.file_id <= 0) return MODULE_ID_INVALID;
  uint32_t target = (uint32_t)span.file_id;
  for (size_t i = 1; i < s->modules_table->count; i++) {
    ModuleId id = (ModuleId){.idx = (uint32_t)i};
    struct ModuleInfo *m = module_info(s, id);
    if (m && input_id_is_valid(m->input) && m->input.idx == target)
      return id;
  }
  return MODULE_ID_INVALID;
}
