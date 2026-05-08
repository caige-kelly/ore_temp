#include "refs.h"

#include <stddef.h>

#include "../../common/arena.h"
#include "../../common/hashmap.h"
#include "../sema.h"

static Vec *get_or_create_list(struct Sema *s, DefId def) {
  if (s->refs_to_def.entries == NULL)
    hashmap_init_in(&s->refs_to_def, s->arena);

  uint64_t key = (uint64_t)def.idx;
  if (hashmap_contains(&s->refs_to_def, key))
    return (Vec *)hashmap_get(&s->refs_to_def, key);

  Vec *list = vec_new_in(s->arena, sizeof(struct NodeId));
  hashmap_put(&s->refs_to_def, key, list);
  return list;
}

void refs_record(struct Sema *s, DefId def, struct NodeId ident_node) {
  if (!def_id_is_valid(def) || ident_node.id == 0)
    return;
  Vec *list = get_or_create_list(s, def);
  vec_push(list, &ident_node);
}

void refs_drop(struct Sema *s, DefId def) {
  if (!def_id_is_valid(def))
    return;
  if (s->refs_to_def.entries == NULL)
    return;
  // We can't actually delete an entry from the existing HashMap
  // (it's append-only by design). Reset the list in place — the
  // map keeps the entry but the list becomes empty.
  uint64_t key = (uint64_t)def.idx;
  if (!hashmap_contains(&s->refs_to_def, key))
    return;
  Vec *list = (Vec *)hashmap_get(&s->refs_to_def, key);
  if (list)
    list->count = 0;
}

Vec *query_references_of(struct Sema *s, DefId def) {
  if (!def_id_is_valid(def) || s->refs_to_def.entries == NULL)
    return NULL;
  uint64_t key = (uint64_t)def.idx;
  if (!hashmap_contains(&s->refs_to_def, key))
    return NULL;
  return (Vec *)hashmap_get(&s->refs_to_def, key);
}
