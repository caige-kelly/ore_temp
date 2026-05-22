#include "collect.h"

#include "../db.h"

// Visit every slot in one per-kind / per-file / per-module slot column,
// skipping the reserved sentinel at index 0. Only the HOT columns are
// walked — the sole caller (db_free's teardown) frees slot->deps, a hot
// field; the cold columns own no heap. Key is 0 — teardown ignores it.
static void visit_slot_column(Vec *col, QueryKind kind, DbSlotVisitor visit,
                              void *ud) {
  for (size_t i = 1; i < col->count; i++) {
    QuerySlotHot *slot = (QuerySlotHot *)vec_get(col, i);
    if (slot)
      visit(slot, kind, 0, ud);
  }
}

void db_for_each_slot(struct db *s, DbSlotVisitor visit, void *user_data) {
  if (!s || !visit)
    return;

  // 1. Per-kind table slot columns. Each table embeds only the slots
  //    its kind can run — the walk is dense, no sparse entries.
  visit_slot_column(&s->fns.slot_type_hot, QUERY_TYPE_OF_DECL, visit,
                    user_data);
  visit_slot_column(&s->fns.slot_signature_hot, QUERY_FN_SIGNATURE, visit,
                    user_data);
  visit_slot_column(&s->fns.slot_infer_hot, QUERY_INFER_BODY, visit, user_data);
  visit_slot_column(&s->fns.slot_body_scopes_hot, QUERY_BODY_SCOPES, visit,
                    user_data);
  visit_slot_column(&s->structs.slot_type_hot, QUERY_TYPE_OF_DECL, visit,
                    user_data);
  visit_slot_column(&s->unions.slot_type_hot, QUERY_TYPE_OF_DECL, visit,
                    user_data);
  visit_slot_column(&s->enums.slot_type_hot, QUERY_TYPE_OF_DECL, visit,
                    user_data);
  visit_slot_column(&s->effects.slot_type_hot, QUERY_TYPE_OF_DECL, visit,
                    user_data);
  visit_slot_column(&s->handlers.slot_type_hot, QUERY_TYPE_OF_DECL, visit,
                    user_data);
  visit_slot_column(&s->variables.slot_type_hot, QUERY_TYPE_OF_DECL, visit,
                    user_data);
  visit_slot_column(&s->constants.slot_type_hot, QUERY_TYPE_OF_DECL, visit,
                    user_data);
  visit_slot_column(&s->constants.slot_const_eval_hot, QUERY_CONST_EVAL, visit,
                    user_data);

  // 2. def_identity / resolve_ref / resolve_path — dense hot slot
  //    columns routed by the *_cache HashMaps.
  visit_slot_column(&s->def_identity.slots_hot, QUERY_DEF_IDENTITY, visit,
                    user_data);
  visit_slot_column(&s->resolve_ref.slots_hot, QUERY_RESOLVE_REF, visit,
                    user_data);
  visit_slot_column(&s->resolve_path.slots_hot, QUERY_RESOLVE_PATH, visit,
                    user_data);

  // 3. Per-file QUERY_FILE_AST slot column.
  for (size_t i = 1; i < s->files.ids.count; i++) {
    if (i >= s->files.slots_ast_hot.count)
      continue;
    FileId *fid = (FileId *)vec_get(&s->files.ids, i);
    visit((QuerySlotHot *)vec_get(&s->files.slots_ast_hot, i), QUERY_FILE_AST,
          (uint64_t)fid->idx, user_data);
  }

  // 4. Per-module derived-query slot columns.
  for (size_t i = 1; i < s->modules.ids.count; i++) {
    if (i >= s->modules.slots_index_hot.count)
      continue;
    ModuleId *mid = (ModuleId *)vec_get(&s->modules.ids, i);
    visit((QuerySlotHot *)vec_get(&s->modules.slots_index_hot, i),
          QUERY_TOP_LEVEL_INDEX, (uint64_t)mid->idx, user_data);
    visit((QuerySlotHot *)vec_get(&s->modules.slots_exports_hot, i),
          QUERY_MODULE_EXPORTS, (uint64_t)mid->idx, user_data);
  }
}
