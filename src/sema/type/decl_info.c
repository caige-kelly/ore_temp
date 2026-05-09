#include "decl_info.h"

#include "../../common/arena.h"
#include "../../common/hashmap.h"
#include "../sema.h"

struct SemaDeclInfo *sema_decl_info(struct Sema *s, DefId def) {
  if (!s || !def_id_is_valid(def))
    return NULL;
  if (s->decl_info.entries == NULL)
    hashmap_init_in(&s->decl_info, &s->arena);

  uint64_t key = (uint64_t)def.idx;
  if (hashmap_contains(&s->decl_info, key))
    return (struct SemaDeclInfo *)hashmap_get(&s->decl_info, key);

  struct SemaDeclInfo *info = arena_alloc(&s->arena, sizeof(struct SemaDeclInfo));
  *info = (struct SemaDeclInfo){0};
  sema_query_slot_init(&info->type_query, QUERY_TYPE_OF_DECL);
  hashmap_put(&s->decl_info, key, info);
  return info;
}
