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
#include "../type/type.h"

// Initial-capacity defaults for the database's main arenas. 64 KiB is
// comfortable for tiny programs and grows on demand. LSP shells can
// override later via a config knob.
#define SEMA_DEFAULT_ARENA_CAP (64 * 1024)
#define SEMA_DEFAULT_PASS_ARENA_CAP (64 * 1024)
#define SEMA_DEFAULT_POOL_CAP 1024
#define SEMA_DEFAULT_SLOT_BUDGET 50000

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
//   * ID tables, inputs subsystem, primitives module
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
  hashmap_init_in(&s->is_comptime_entries, &s->arena);
  hashmap_init_in(&s->layout_of_type, &s->arena);
  hashmap_init_in(&s->decl_info, &s->arena);
  hashmap_init_in(&s->resolve_ref_entries, &s->arena);
  hashmap_init_in(&s->resolve_path_entries, &s->arena);
  hashmap_init_in(&s->type_of_expr_entries, &s->arena);
  hashmap_init_in(&s->fn_signatures, &s->arena);
  hashmap_init_in(&s->param_locators, &s->arena);
  hashmap_init_in(&s->struct_signatures, &s->arena);
  hashmap_init_in(&s->field_locators, &s->arena);
  hashmap_init_in(&s->enum_signatures, &s->arena);
  hashmap_init_in(&s->variant_locators, &s->arena);

  // R4 — unified intern pool. Reserved indices are populated by
  // ip_init; sema_types_init hooks the primitive Type*s to their
  // reserved IpIndex slots. types_by_ip is the bridge lookup
  // (IpIndex.v → struct Type*) — sparse, grows as compound types
  // get registered.
  ip_init(&s->intern_pool);
  s->types_by_ip = vec_new_in(&s->arena, sizeof(struct Type *));

  // Pre-intern builtin / hot-path names so dispatch can compare a
  // single uint32_t instead of pool_get + char-by-char strcmp. Only
  // names that actually have an active dispatch site are kept; the
  // rest were declared but never wired and have been removed.
  s->name_sizeOf = pool_intern(&s->pool, "sizeOf", 6);
  s->name_alignOf = pool_intern(&s->pool, "alignOf", 7);
  s->name_TypeOf = pool_intern(&s->pool, "TypeOf", 6);
  s->name_intCast = pool_intern(&s->pool, "intCast", 7);
  s->name_typeName = pool_intern(&s->pool, "typeName", 8);

  sema_ids_init(s);
  sema_types_init(s);
  sema_inputs_init(s);
  primitives_init(s);
}

void sema_free(struct Sema *s) {
  if (!s)
    return;
  // DiagBag entries live in the arena — no separate free.
  sourcemap_free_sources(&s->source_map);
  pool_free(&s->pool);
  ip_free(&s->intern_pool);
  arena_free(&s->pass_arena);
  arena_free(&s->arena);
  *s = (struct Sema){0};
}
