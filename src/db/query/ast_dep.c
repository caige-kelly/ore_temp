#include "ast_dep.h"

#include "../db.h"
#include "query.h"

// Resolve a DefId's owning ModuleId via the defs.parent_modules direct
// column — one cache-line read on this hot path. Returns MODULE_ID_NONE
// if the def is invalid or the column entry was never stamped.
static ModuleId owning_module_of(struct db *s, DefId def) {
  if (!def_id_valid(def))
    return MODULE_ID_NONE;
  if (def.idx >= s->defs.parent_modules.count)
    return MODULE_ID_NONE;
  ModuleId *mid = (ModuleId *)vec_get(&s->defs.parent_modules, def.idx);
  return mid ? *mid : MODULE_ID_NONE;
}

// Trigger an AST-dep recording on the current query's frame. The AST
// slot is an LSP-managed input — db_query_begin returns CACHED and
// record_dep_on_parent (inside db_query_begin) stamps the dep on our
// caller's frame. We use &mod->id as the key because it's pointer-stable
// for the ModuleInfo's lifetime (it lives in db.arena).
static void record_dep_on_module(struct db *s, ModuleId mid) {
  ModuleId *stable_mid = (ModuleId *)vec_get(&s->modules.ids, mid.idx);
  if (!stable_mid)
    return;
  (void)db_query_begin(s, QUERY_MODULE_AST, stable_mid);
  // Note: caller is responsible for being inside a query frame. If they
  // aren't, record_dep_on_parent inside db_query_begin no-ops (it checks
  // s->query_stack.count). No state cleanup needed here — db_query_begin's
  // cached path doesn't push a frame.
}

void db_record_ast_dep_for_def(struct db *s, DefId def) {
  if (!s)
    return;
  ModuleId mid = owning_module_of(s, def);
  if (!module_id_valid(mid))
    return;
  record_dep_on_module(s, mid);
}

void db_record_ast_dep_for_span(struct db *s, TinySpan span) {
  if (!s)
    return;
  ModuleId mid = db_module_for_file(s, file_id_make_physical(span_file(span)));
  if (!module_id_valid(mid))
    return;
  record_dep_on_module(s, mid);
}
