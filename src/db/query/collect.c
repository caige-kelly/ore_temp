#include "collect.h"

#include "../db.h"

// Walk one SoA slot column (one entry per Def/Scope). Skips the NONE
// sentinel at idx 0. Key is a stack temp typed to the id kind — the
// visitor must not store the pointer past its own return.
#define VISIT_DEF_COLUMN(s, col, qkind, visit, ud)                             \
  do {                                                                         \
    for (size_t i = 1; i < (s)->defs.col.count; i++) {                         \
      QuerySlot *slot = (QuerySlot *)vec_get(&(s)->defs.col, i);               \
      if (!slot)                                                               \
        continue;                                                              \
      DefId key = {.idx = (uint32_t)i};                                        \
      (visit)(slot, (qkind), &key, (ud));                                      \
    }                                                                          \
  } while (0)

// HashMap visitor adapter — extracts the embedded slot from a
// ResolvePathEntry and forwards to the user's visitor.
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

  // 1. Per-Def SoA columns.
  VISIT_DEF_COLUMN(s, slots_type, QUERY_TYPE_OF_DECL, visit, user_data);
  VISIT_DEF_COLUMN(s, slots_signature, QUERY_FN_SIGNATURE, visit, user_data);
  VISIT_DEF_COLUMN(s, slots_const_eval, QUERY_CONST_EVAL, visit, user_data);
  VISIT_DEF_COLUMN(s, slots_infer, QUERY_INFER_BODY, visit, user_data);

  // (No per-scope SoA slot columns: QUERY_RESOLVE_REF moved to a
  //  HashMap-keyed cache for per-(scope, name) precision.)

  // 2. HashMap-embedded slots: resolve_path, def_by_identity, resolve_ref_cache.
  struct map_visit_ctx ctx = {.visit = visit, .user_data = user_data};
  hashmap_foreach(&s->resolve_path, visit_resolve_path_entry, &ctx);
  hashmap_foreach(&s->def_by_identity, visit_def_identity_entry, &ctx);
  hashmap_foreach(&s->resolve_ref_cache, visit_resolve_ref_entry, &ctx);

  // 4. Per-file QUERY_FILE_AST slot column. Key is a pointer to the
  //    file's own FileId stored in files.ids[i] — pointer-stable for
  //    the db's lifetime so dep entries can dereference it later during
  //    revalidation.
  for (size_t i = 1; i < s->files.ids.count; i++) {
    if (i >= s->files.slots_ast.count)
      continue;
    FileId *fid = (FileId *)vec_get(&s->files.ids, i);
    visit((QuerySlot *)vec_get(&s->files.slots_ast, i), QUERY_FILE_AST, fid,
          user_data);
  }

  // 5. Per-module derived-query slot columns (thin aggregate). Key is
  //    a pointer to the module's own ModuleId in modules.ids[i].
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
