#include "refs.h"

#include <stddef.h>

#include "../../common/arena.h"
#include "../../common/hashmap.h"
#include "../sema.h"

static Vec *get_or_create_list(struct Sema *s, DefId def) {
  if (s->refs_to_def.entries == NULL)
    hashmap_init_in(&s->refs_to_def, &s->arena);

  uint64_t key = (uint64_t)def.idx;
  if (hashmap_contains(&s->refs_to_def, key))
    return (Vec *)hashmap_get(&s->refs_to_def, key);

  Vec *list = vec_new_in(&s->arena, sizeof(struct NodeId));
  hashmap_put_or_die(&s->refs_to_def, key, list, "refs_to_def");
  return list;
}

static Vec *get_existing_list(struct Sema *s, DefId def) {
  if (!def_id_is_valid(def) || s->refs_to_def.entries == NULL)
    return NULL;
  uint64_t key = (uint64_t)def.idx;
  if (!hashmap_contains(&s->refs_to_def, key))
    return NULL;
  return (Vec *)hashmap_get(&s->refs_to_def, key);
}

void refs_record(struct Sema *s, DefId def, struct NodeId ident_node) {
  if (!def_id_is_valid(def) || ident_node.id == 0)
    return;
  Vec *list = get_or_create_list(s, def);
  vec_push(list, &ident_node);
}

void refs_unrecord(struct Sema *s, DefId def, struct NodeId ident_node) {
  if (!def_id_is_valid(def) || ident_node.id == 0)
    return;
  Vec *list = get_existing_list(s, def);
  if (!list)
    return;

  // Linear scan + swap-remove. The order of entries doesn't matter
  // to consumers (LSP find-references presents them sorted by span
  // anyway), so swap-with-last is the cheap stable option.
  for (size_t i = 0; i < list->count; i++) {
    struct NodeId *n = (struct NodeId *)vec_get(list, i);
    if (n && n->id == ident_node.id) {
      if (i != list->count - 1) {
        struct NodeId *last =
            (struct NodeId *)vec_get(list, list->count - 1);
        *n = *last;
      }
      list->count--;
      return;
    }
  }
}

void refs_drop(struct Sema *s, DefId def) {
  Vec *list = get_existing_list(s, def);
  if (list)
    list->count = 0;
}

Vec *query_references_of(struct Sema *s, DefId def) {
  return get_existing_list(s, def);
}
