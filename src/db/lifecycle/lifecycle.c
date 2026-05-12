#include "../db.h"

#include "../../support/common/arena.h"
#include "../../support/common/hashmap.h"
#include "../../support/common/stringpool.h"
#include "../../support/common/vec.h"
#include "../../support/diag/diag.h"
#include "../../support/diag/sourcemap.h"
#include "../ids/ids.h"
#include "../query/query.h"
//#include "../../sema/type/type.h"
//#include "../../sema/modules/inputs.h"
//#include "../../sema/modules/modules.h"


//Initial-capacity defaults for the database's main arenas.
#define SEMA_DEFAULT_ARENA_CAP (64 * 1024)
#define SEMA_DEFAULT_PASS_ARENA_CAP (64 * 1024)
#define SEMA_DEFAULT_POOL_CAP 1024

/*
  Db lifecycle.

  `db_init` is the single instantiation used by every consumer.
  It owns:

    * arenas + pool + diags + source map
    * query stack
    * core hashmaps that aren't lazy-inited at first use
    * ID tables, inputs subsystem, primitives module

  `sema_free` releases everything allocated in the arena, 
  so dropping it tears the whole database down in one shot.
*/

void db_init(struct db *s) {
  *s = (struct db){0};

  arena_init(&s->arena, SEMA_DEFAULT_ARENA_CAP);
  arena_init(&s->pass_arena, SEMA_DEFAULT_PASS_ARENA_CAP);
  pool_init(&s->pool, SEMA_DEFAULT_POOL_CAP);
  s->source_map = sourcemap_new(&s->arena, &s->pool);
  s->diags = diag_bag_new(&s->arena);

  vec_init_in(&s->query_stack, &s->arena, sizeof(struct QueryFrame));

  hashmap_init_in(&s->module_by_path, &s->arena);
  hashmap_init_in(&s->resolve_ref_entries, &s->arena);
  hashmap_init_in(&s->resolve_path_entries, &s->arena);
  hashmap_init_in(&s->struct_field_defs, &s->arena);
  hashmap_init_in(&s->enum_variant_defs, &s->arena);

  // unified intern pool.
  ip_init(&s->intern_pool);

  // Pre-intern builtin
  s->name_sizeOf = pool_intern(&s->pool, "sizeOf", 6);
  s->name_alignOf = pool_intern(&s->pool, "alignOf", 7);
  s->name_TypeOf = pool_intern(&s->pool, "TypeOf", 6);
  s->name_intCast = pool_intern(&s->pool, "intCast", 7);
  s->name_typeName = pool_intern(&s->pool, "typeName", 8);

  db_ids_init(s);
  // sema_types_init(s);
  // sema_inputs_init(s);
  // primitives_init(s);
}

void db_free(struct db *s) {
  if (!s)
    return;
  // DiagBag entries live in the arena — no separate free.
  sourcemap_free_sources(&s->source_map);
  pool_free(&s->pool);
  ip_free(&s->intern_pool);
  arena_free(&s->pass_arena);
  arena_free(&s->arena);
  *s = (struct db){0};
}
