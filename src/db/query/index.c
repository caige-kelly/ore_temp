#include "query.h"
#include "query_engine.h"
#include "ast.h"
#include "../../db/db.h"
#include "../../db/workspace/module_info.h"

Fingerprint db_query_top_level_index(struct db *s, ModuleId mod) {
    ModuleId *stable_mod = (ModuleId*)vec_get(&s->modules.ids, mod.idx);
    
    QuerySlot *slot = db_locate_slot(s, QUERY_TOP_LEVEL_INDEX, stable_mod);
    
    DB_QUERY_GUARD(s, QUERY_TOP_LEVEL_INDEX, stable_mod, 
                   slot->fingerprint, 
                   FINGERPRINT_NONE, 
                   FINGERPRINT_NONE);
    
    // 1. Ensure AST is parsed
    db_query_module_ast(s, mod);
    
    // 2. Get the index populated by the parser
    Vec *idx = (Vec*)vec_get(&s->modules.top_level_indices, mod.idx);
    
    // 3. Compute fingerprint over the index content (names, vis, ast_ids)
    //    We explicitly EXCLUDE node IDs and Spans from the fingerprint
    //    so that body-only edits or line shifts don't invalidate the index result.
    Fingerprint fp = db_fp_u64(idx->count);
    for (size_t i = 0; i < idx->count; i++) {
        TopLevelEntry *e = (TopLevelEntry*)vec_get(idx, i);
        fp = db_fp_combine(fp, db_fp_u64(e->name.idx));
        uint64_t flags = ((uint64_t)e->vis << 1);
        fp = db_fp_combine(fp, db_fp_u64(flags));
        fp = db_fp_combine(fp, db_fp_u64(e->ast_id.idx));
    }
    
    db_query_succeed(s, QUERY_TOP_LEVEL_INDEX, stable_mod, fp);
    return fp;
}

static QuerySlot* get_or_create_def_slot(struct db *s, ModuleId mod, StrId name) {
    HashMap **map_ptr = (HashMap**)vec_get(&s->modules.def_maps, mod.idx);
    if (!*map_ptr) {
        *map_ptr = arena_alloc(&s->arena, sizeof(HashMap));
        hashmap_init_in(*map_ptr, &s->arena);
    }
    
    QuerySlot *slot = (QuerySlot*)hashmap_get(*map_ptr, (uint64_t)name.idx);
    if (!slot) {
        slot = arena_alloc(&s->arena, sizeof(QuerySlot));
        db_query_slot_init(slot, QUERY_DEF_FOR_NAME);
        hashmap_put_or_die(*map_ptr, (uint64_t)name.idx, slot, "def_maps");
    }
    return slot;
}

DefId db_query_def_for_name(struct db *s, ModuleId mod, StrId name) {
    DefForNameKey key = { .mod = mod, .name = name };
    QuerySlot *slot = get_or_create_def_slot(s, mod, name);
    
    DB_QUERY_GUARD(s, QUERY_DEF_FOR_NAME, &key, 
                   (DefId){.idx = (uint32_t)slot->fingerprint}, // We store DefId.idx in fp for now? No.
                   DEF_ID_NONE, 
                   DEF_ID_NONE);
    
    // 1. Depend on index
    db_query_top_level_index(s, mod);
    
    // 2. Find in index
    Vec *idx = (Vec*)vec_get(&s->modules.top_level_indices, mod.idx);
    TopLevelEntry *entry = NULL;
    for (size_t i = 0; i < idx->count; i++) {
        TopLevelEntry *e = (TopLevelEntry*)vec_get(idx, i);
        if (e->name.idx == name.idx) {
            entry = e;
            break;
        }
    }
    
    if (!entry) {
        db_query_fail(s, QUERY_DEF_FOR_NAME, &key);
        return DEF_ID_NONE;
    }
    
    // 3. Allocate/Lookup DefId
    // In a real implementation, we'd check if we already have a DefId for this AstId.
    // For now, we'll just allocate a new one if it's the first time.
    // TODO: Stable DefId allocation via AstIdMap
    DefId def = db_alloc_def(s);
    
    // Populate Def SoA
    *(StrId*)vec_get(&s->defs.names, def.idx) = name;
    *(AstId*)vec_get(&s->defs.ast_ids, def.idx) = entry->ast_id;
    // ... other fields
    
    // 4. Succeed
    // Result is the DefId. We'll use the DefId.idx as the fingerprint for simplicity in this example,
    // but a real fingerprint should include visibility etc.
    Fingerprint fp = (Fingerprint)def.idx;
    db_query_succeed(s, QUERY_DEF_FOR_NAME, &key, fp);
    return def;
}
