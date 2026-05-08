#include "../sema.h"

#include "../../common/arena.h"
#include "../../common/hashmap.h"
#include "../../common/stringpool.h"
#include "../../common/vec.h"
#include "../../diag/diag.h"
#include "../../diag/sourcemap.h"
#include "../ids/ids.h"
#include "../modules/inputs.h"
#include "../modules/modules.h"
#include "../query/query.h"

// Initial-capacity defaults for the database's main arenas. 64 KiB is
// comfortable for tiny programs and grows on demand. LSP shells can
// override later via a config knob.
#define SEMA_DEFAULT_ARENA_CAP      (64 * 1024)
#define SEMA_DEFAULT_PASS_ARENA_CAP (64 * 1024)
#define SEMA_DEFAULT_POOL_CAP       1024
#define SEMA_DEFAULT_SLOT_BUDGET    50000

// Sema lifecycle.
//
// `sema_init` is the single bring-up sequence used by every consumer
// (CLI driver, LSP shell). It owns the storage previously split
// across `Compiler` (arena/pool/diags/source_map/pass_arena) plus
// every initialization the database needs:
//
//   * arenas + pool + diags + source map
//   * query stack
//   * core hashmaps that aren't lazy-inited at first use
//   * ID tables, inputs subsystem, prelude module
//
// `sema_free` releases the arenas and pool. Everything else allocated
// by sema lives in the arena, so dropping it tears the whole database
// down in one shot.

void sema_init(struct Sema *s) {
  *s = (struct Sema){0};

  arena_init(&s->arena, SEMA_DEFAULT_ARENA_CAP);
  arena_init(&s->pass_arena, SEMA_DEFAULT_PASS_ARENA_CAP);
  pool_init(&s->pool, SEMA_DEFAULT_POOL_CAP);
  s->source_map = sourcemap_new(&s->arena, &s->pool);
  s->diags = diag_bag_new(&s->arena);

  s->slot_budget = SEMA_DEFAULT_SLOT_BUDGET;
  s->query_stack = vec_new_in(&s->arena, sizeof(struct QueryFrame));

  hashmap_init_in(&s->module_by_path, &s->arena);
  hashmap_init_in(&s->const_eval_entries, &s->arena);

  sema_ids_init(s);
  sema_inputs_init(s);
  prelude_init(s);
}

void sema_free(struct Sema *s) {
  if (!s)
    return;
  // DiagBag entries live in the arena — no separate free.
  sourcemap_free_sources(&s->source_map);
  pool_free(&s->pool);
  arena_free(&s->pass_arena);
  arena_free(&s->arena);
  *s = (struct Sema){0};
}
