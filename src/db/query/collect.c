#include "collect.h"

#include "../db.h"
#include "../workspace/module_info.h"

// Walk one SoA slot column (one entry per Def/Scope). Skips the NONE
// sentinel at idx 0. Key is a stack temp typed to the id kind — the
// visitor must not store the pointer past its own return.
#define VISIT_DEF_COLUMN(s, col, qkind, visit, ud) do {                    \
    for (size_t i = 1; i < (s)->defs.col.count; i++) {                     \
        QuerySlot *slot = (QuerySlot *)vec_get(&(s)->defs.col, i);         \
        if (!slot) continue;                                               \
        DefId key = {.idx = (uint32_t)i};                                  \
        (visit)(slot, (qkind), &key, (ud));                                \
    }                                                                      \
} while (0)

#define VISIT_SCOPE_COLUMN(s, col, qkind, visit, ud) do {                  \
    for (size_t i = 1; i < (s)->scopes.col.count; i++) {                   \
        QuerySlot *slot = (QuerySlot *)vec_get(&(s)->scopes.col, i);       \
        if (!slot) continue;                                               \
        ScopeId key = {.idx = (uint32_t)i};                                \
        (visit)(slot, (qkind), &key, (ud));                                \
    }                                                                      \
} while (0)

// HashMap visitor adapter — extracts the embedded slot from a
// ResolvePathEntry and forwards to the user's visitor.
struct resolve_path_ctx {
    DbSlotVisitor visit;
    void          *user_data;
};

static bool visit_resolve_path_entry(uint64_t key, void *value, void *ud_raw) {
    struct resolve_path_ctx *ctx = (struct resolve_path_ctx *)ud_raw;
    ResolvePathEntry *entry = (ResolvePathEntry *)value;
    if (!entry) return true;
    StrId path = {.idx = (uint32_t)key};
    ctx->visit(&entry->slot, QUERY_RESOLVE_PATH, &path, ctx->user_data);
    return true;
}

void db_for_each_slot(struct db *s, DbSlotVisitor visit, void *user_data) {
    if (!s || !visit) return;

    // 1. Per-Def SoA columns.
    VISIT_DEF_COLUMN(s, slots_type,         QUERY_TYPE_OF_DECL, visit, user_data);
    VISIT_DEF_COLUMN(s, slots_signature,    QUERY_FN_SIGNATURE, visit, user_data);
        VISIT_DEF_COLUMN(s, slots_const_eval,   QUERY_CONST_EVAL,   visit, user_data);

    // 2. Per-Scope SoA column.
    VISIT_SCOPE_COLUMN(s, slots_resolve_ref, QUERY_RESOLVE_REF, visit, user_data);

    // 3. HashMap-embedded slots: resolve_path.
    struct resolve_path_ctx ctx = { .visit = visit, .user_data = user_data };
    hashmap_foreach(&s->resolve_path, visit_resolve_path_entry, &ctx);

    // 4. Per-module embedded slots. Each ModuleInfo lives in db.arena
    //    (pointer-stable); we walk db.modules to find them. Key for
    //    db_locate_slot purposes is the ModuleInfo's own .id field —
    //    pointer-stable for the ModuleInfo's lifetime.
    for (size_t i = 1; i < s->modules.files.count; i++) {
        if (i >= s->modules.slots_ast.count) continue;
        ModuleId *mid = (ModuleId*)vec_get(&s->modules.ids, i);
        visit((QuerySlot*)vec_get(&s->modules.slots_ast, i),       QUERY_MODULE_AST,      mid, user_data);
        visit((QuerySlot*)vec_get(&s->modules.slots_index, i),  QUERY_TOP_LEVEL_INDEX, mid, user_data);
        visit((QuerySlot*)vec_get(&s->modules.slots_exports, i),   QUERY_MODULE_EXPORTS,  mid, user_data);
        // visit(&mod->slot_module_def_map,   QUERY_MODULE_DEF_MAP,  &mod->id, user_data); // removed since slots_def_map doesn't exist
    }
}
