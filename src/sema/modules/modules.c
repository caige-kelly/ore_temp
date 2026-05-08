#include "modules.h"

#include <stddef.h>

#include "../../common/arena.h"
#include "../../common/hashmap.h"
#include "../../parser/ast.h"
#include "../../project/module_loader.h"
#include "../query/query_engine.h"
#include "../sema.h"
#include "def_map.h"

ModuleId module_create(struct Sema *s, InputId input, bool is_prelude) {
  struct ModuleInfo *info = arena_alloc(s->arena, sizeof(struct ModuleInfo));
  *info = (struct ModuleInfo){
      .input = input,
      .ast = NULL,
      .ast_fp = FINGERPRINT_NONE,
      .internal_scope = SCOPE_ID_INVALID,
      .export_scope = SCOPE_ID_INVALID,
      .imports = NULL,
      .is_prelude = is_prelude,
      .resolving = false,
      .resolved = false,
  };
  sema_query_slot_init(&info->def_map_query, QUERY_MODULE_DEF_MAP);
  sema_query_slot_init(&info->exports_query, QUERY_MODULE_EXPORTS);

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
  // Prelude has no source; its def_map is built directly by
  // prelude_init without going through the parser.
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
  Vec *ast = parse_source(s->arena, s->arena, s->pool, s->diags,
                          (int)m->input.idx, source, ii->source_len);
  if (!ast) {
    sema_query_fail(s, &ii->ast_query);
    return NULL;
  }

  m->ast = ast;
  // Light-weight fingerprint: the AST root vector pointer is unique
  // per parse (fresh allocation), so its address suffices to detect
  // re-parse cascades. A structural hash (node count + leading
  // NodeId range) is a future polish; the source-text fingerprint on
  // InputInfo already gates whether we re-parse at all.
  m->ast_fp = query_fingerprint_from_pointer(ast);
  query_slot_set_fingerprint(&ii->ast_query, m->ast_fp);
  ii->is_dirty = false;

  sema_query_succeed(s, &ii->ast_query);
  return ast;
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

  // Lazily create the scopes the moment def_map runs. Internal scope
  // parents to the prelude's exports for primitive lookups; the
  // prelude itself has no parent.
  if (!scope_id_is_valid(m->internal_scope)) {
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

  m->resolving = true;
  bool ok = def_map_collect_top_level(s, mid);
  m->resolving = false;

  if (ok) {
    m->resolved = true;
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
  if (!m->resolved && !query_module_def_map(s, mid))
    return SCOPE_ID_INVALID;
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
