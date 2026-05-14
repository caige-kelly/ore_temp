#include "invalidate.h"
#include "../db.h"
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
