#include "invalidate.h"
#include "../db.h"
#include "../workspace/module_info.h"
#include "../request/request.h"

QuerySlot* db_locate_slot(struct db *s, QueryKind kind, const void *key) {
    if (!key) return NULL;
    switch (kind) {
        case QUERY_TYPE_OF_DECL:
            return (QuerySlot*)vec_get(&s->defs.slots_type, ((DefId*)key)->idx);
        case QUERY_FN_SIGNATURE:
            return (QuerySlot*)vec_get(&s->defs.slots_signature, ((DefId*)key)->idx);
        case QUERY_IS_COMPTIME:
            return (QuerySlot*)vec_get(&s->defs.slots_is_comptime, ((DefId*)key)->idx);
        case QUERY_CONST_EVAL:
            return (QuerySlot*)vec_get(&s->defs.slots_const_eval, ((DefId*)key)->idx);
        case QUERY_RESOLVE_REF:
            return (QuerySlot*)vec_get(&s->scopes.slots_resolve_ref, ((ScopeId*)key)->idx);

        // Per-module slots live on ModuleInfo (pointer-stable in db.arena).
        // Key is ModuleId; the slot's home is &mod->slot_<kind>.
        case QUERY_MODULE_AST:
        case QUERY_TOP_LEVEL_INDEX:
        case QUERY_MODULE_EXPORTS:
        case QUERY_MODULE_DEF_MAP: {
            ModuleId mid = *(const ModuleId *)key;
            struct ModuleInfo *mod = db_get_module(s, mid);
            if (!mod) return NULL;
            switch (kind) {
                case QUERY_MODULE_AST:      return &mod->slot_module_ast;
                case QUERY_TOP_LEVEL_INDEX: return &mod->slot_top_level_index;
                case QUERY_MODULE_EXPORTS:  return &mod->slot_module_exports;
                case QUERY_MODULE_DEF_MAP:  return &mod->slot_module_def_map;
                default:                    return NULL;
            }
        }

        // Sparse-keyed via HashMap. Key is a StrId pointer (the interned
        // dotted-path). Embedded ResolvePathEntry lives in db.arena —
        // pointer-stable for db lifetime.
        case QUERY_RESOLVE_PATH: {
            StrId path = *(const StrId *)key;
            ResolvePathEntry *entry = (ResolvePathEntry *)hashmap_get(
                &s->resolve_path, (uint64_t)path.idx);
            return entry ? &entry->slot : NULL;
        }

        default:
            return NULL;
    }
}

RevalidateResult db_revalidate(struct db *s, QuerySlot *slot) {
    if (!slot) return DB_REVALIDATE_RECOMPUTE;
    
    if (slot->state != QUERY_DONE && slot->state != QUERY_ERROR)
        return DB_REVALIDATE_RECOMPUTE;

    uint64_t eff = db_effective_revision(s);
    if (slot->verified_rev == eff)
        return DB_REVALIDATE_SKIP_RECOMPUTE;

    if (slot->has_untracked_read) {
#ifdef ORE_DEBUG_QUERIES
        s->query_stats[(int)slot->kind].recompute_due_to_untracked++;
#endif
        return DB_REVALIDATE_RECOMPUTE;
    }

    if (slot->deps) {
        for (size_t i = 0; i < slot->deps->count; i++) {
            QueryDep *dep = (QueryDep *)vec_get(slot->deps, i);
            if (!dep) continue;

            QuerySlot *dep_slot = db_locate_slot(s, dep->kind, dep->key);
            if (!dep_slot) return DB_REVALIDATE_RECOMPUTE;

            RevalidateResult dep_result = db_revalidate(s, dep_slot);
            if (dep_result == DB_REVALIDATE_RECOMPUTE)
                return DB_REVALIDATE_RECOMPUTE;

            if (dep_slot->state != QUERY_DONE)
                return DB_REVALIDATE_RECOMPUTE;
            if (dep_slot->fingerprint != dep->dep_fp)
                return DB_REVALIDATE_RECOMPUTE;
        }
    }

    slot->verified_rev = eff;
    return DB_REVALIDATE_SKIP_RECOMPUTE;
}
