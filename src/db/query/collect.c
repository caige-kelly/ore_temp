#include "collect.h"

#include "../db.h"

// Visit every slot in one per-kind / per-file / per-module slot column,
// skipping the reserved sentinel at index 0. The key is NULL — the only
// caller (db_free's teardown) ignores kind/key.
static void visit_slot_column(Vec *col, QueryKind kind, DbSlotVisitor visit,
                              void *ud) {
  for (size_t i = 1; i < col->count; i++) {
    QuerySlot *slot = (QuerySlot *)vec_get(col, i);
    if (slot)
      visit(slot, kind, NULL, ud);
  }
}

// HashMap visitor adapter — extracts the embedded slot from an entry and
// forwards to the user's visitor.
struct map_visit_ctx {
  DbSlotVisitor visit;
  void *user_data;
};

static bool visit_resolve_path_entry(uint64_t key, void *value, void *ud_raw) {
  struct map_visit_ctx *ctx = (struct map_visit_ctx *)ud_raw;
  ResolvePathEntry *entry = (ResolvePathEntry *)value;
  if (!entry)
    return true;
  StrId path = {.idx = (uint32_t)key};
  ctx->visit(&entry->slot, QUERY_RESOLVE_PATH, &path, ctx->user_data);
  return true;
}

static bool visit_def_identity_entry(uint64_t key, void *value, void *ud_raw) {
  (void)key;
  struct map_visit_ctx *ctx = (struct map_visit_ctx *)ud_raw;
  DefIdentityEntry *entry = (DefIdentityEntry *)value;
  if (!entry)
    return true;
  // Key is the embedded uint64_t — pointer-stable for db lifetime
  // because the entry lives in db.arena.
  ctx->visit(&entry->slot, QUERY_DEF_IDENTITY, &entry->key, ctx->user_data);
  return true;
}

static bool visit_resolve_ref_entry(uint64_t key, void *value, void *ud_raw) {
  (void)key;
  struct map_visit_ctx *ctx = (struct map_visit_ctx *)ud_raw;
  ResolveRefEntry *entry = (ResolveRefEntry *)value;
  if (!entry)
    return true;
  ctx->visit(&entry->slot, QUERY_RESOLVE_REF, &entry->key, ctx->user_data);
  return true;
}

void db_for_each_slot(struct db *s, DbSlotVisitor visit, void *user_data) {
  if (!s || !visit)
    return;

  // 1. Per-kind table slot columns. Each table embeds only the slots
  //    its kind can run — the walk is dense, no sparse entries.
  visit_slot_column(&s->fns.slot_type, QUERY_TYPE_OF_DECL, visit, user_data);
  visit_slot_column(&s->fns.slot_signature, QUERY_FN_SIGNATURE, visit,
                    user_data);
  visit_slot_column(&s->fns.slot_infer, QUERY_INFER_BODY, visit, user_data);
  visit_slot_column(&s->fns.slot_body_scopes, QUERY_BODY_SCOPES, visit,
                    user_data);
  visit_slot_column(&s->structs.slot_type, QUERY_TYPE_OF_DECL, visit,
                    user_data);
  visit_slot_column(&s->unions.slot_type, QUERY_TYPE_OF_DECL, visit, user_data);
  visit_slot_column(&s->enums.slot_type, QUERY_TYPE_OF_DECL, visit, user_data);
  visit_slot_column(&s->effects.slot_type, QUERY_TYPE_OF_DECL, visit,
                    user_data);
  visit_slot_column(&s->handlers.slot_type, QUERY_TYPE_OF_DECL, visit,
                    user_data);
  visit_slot_column(&s->variables.slot_type, QUERY_TYPE_OF_DECL, visit,
                    user_data);
  visit_slot_column(&s->constants.slot_type, QUERY_TYPE_OF_DECL, visit,
                    user_data);
  visit_slot_column(&s->constants.slot_const_eval, QUERY_CONST_EVAL, visit,
                    user_data);

  // 2. HashMap-embedded slots: resolve_path, def_by_identity, resolve_ref.
  struct map_visit_ctx ctx = {.visit = visit, .user_data = user_data};
  hashmap_foreach(&s->resolve_path, visit_resolve_path_entry, &ctx);
  hashmap_foreach(&s->def_by_identity, visit_def_identity_entry, &ctx);
  hashmap_foreach(&s->resolve_ref_cache, visit_resolve_ref_entry, &ctx);

  // 3. Per-file QUERY_FILE_AST slot column. Key is a pointer to the
  //    file's own FileId in files.ids[i] — pointer-stable for the db's
  //    lifetime.
  for (size_t i = 1; i < s->files.ids.count; i++) {
    if (i >= s->files.slots_ast.count)
      continue;
    FileId *fid = (FileId *)vec_get(&s->files.ids, i);
    visit((QuerySlot *)vec_get(&s->files.slots_ast, i), QUERY_FILE_AST, fid,
          user_data);
  }

  // 4. Per-module derived-query slot columns. Key is a pointer to the
  //    module's own ModuleId in modules.ids[i].
  for (size_t i = 1; i < s->modules.ids.count; i++) {
    if (i >= s->modules.slots_index.count)
      continue;
    ModuleId *mid = (ModuleId *)vec_get(&s->modules.ids, i);
    visit((QuerySlot *)vec_get(&s->modules.slots_index, i),
          QUERY_TOP_LEVEL_INDEX, mid, user_data);
    visit((QuerySlot *)vec_get(&s->modules.slots_exports, i),
          QUERY_MODULE_EXPORTS, mid, user_data);
  }
}
