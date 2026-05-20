#include "query.h"

#include <assert.h>

#include "../db.h"
#include "../request/request.h"
#include "invalidate.h"

void db_query_slot_init(QuerySlot *slot, QueryKind kind) {
  *slot = (QuerySlot){
      .state = QUERY_EMPTY,
      .kind = kind,
      .fingerprint = FINGERPRINT_NONE,
      .last_fingerprint = FINGERPRINT_NONE,
      .computed_rev = 0,
      .verified_rev = 0,
      .changed_rev = 0,
      .last_accessed_rev = 0,
      .deps = NULL,
      .diags = NULL,
      .diag_arena = NULL,
      .diag_error_count = 0,
      .has_untracked_read = false,
      .durability = DUR_LOW, // conservative until succeed proves higher
      .revalidating = false,
  };
}

// Append a QueryDep onto a parent frame, lazy-allocating the parent's
// deps Vec if needed. The frame is always re-fetched via vec_get so
// the lookup survives any prior query_stack realloc; the Vec object
// itself lives in db.arena (pointer-stable) and only its backing buffer
// is malloc-owned.
//
// parent_offset = 0 means "the current top of stack" — used from
// db_query_begin on cached returns (caller's frame is still on top
// because the cached path doesn't push a new frame).
//
// parent_offset = 1 means "one below the top" — used from
// db_query_succeed/fail, where the finishing query's frame is on top
// and we want to record its result onto its parent.
static void record_dep_on_parent(struct db *s, QueryKind child_kind,
                                 const void *child_key, Fingerprint child_fp,
                                 size_t parent_offset) {
  if (s->query_stack.count <= parent_offset)
    return;
  QueryFrame *parent = (QueryFrame *)vec_get(
      &s->query_stack, s->query_stack.count - 1 - parent_offset);
  if (!parent->deps) {
    parent->deps = arena_alloc(&s->arena, sizeof(Vec));
    vec_init(parent->deps, sizeof(QueryDep));
  }
  // Dedup: if the parent already has a (kind, key) match in its dep
  // list, just refresh its dep_fp (the child's current fingerprint).
  // Sema's typical pattern — walking many nodes that share an owner —
  // would otherwise push the same (kind, key) hundreds of times,
  // ballooning deps and slowing db_revalidate's per-dep walk. Deps
  // stay small in practice so linear scan is the right structure.
  QueryDep *deps = (QueryDep *)parent->deps->data;
  for (size_t i = 0; i < parent->deps->count; i++) {
    if (deps[i].kind == child_kind && deps[i].key == child_key) {
      deps[i].dep_fp = child_fp;
      goto durability_fold;
    }
  }
  QueryDep dep = {.kind = child_kind, .key = child_key, .dep_fp = child_fp};
  vec_push(parent->deps, &dep);
durability_fold:;

  // Fold the child's durability into the parent's min accumulator. The
  // child slot's durability is final here: cached (offset 0) or
  // just-succeeded (offset 1). Parent durability = MIN over deps.
  QuerySlot *child = db_locate_slot(s, child_kind, child_key);
  if (child) {
    if (!parent->dur_set || child->durability < parent->min_input_dur)
      parent->min_input_dur = child->durability;
    parent->dur_set = true;
  }
}

// Push a frame for a query that just transitioned to RUNNING. The
// frame inherits slot->deps so a recompute reuses the prior Vec object
// + malloc buffer (record_dep_on_parent finds parent->deps non-NULL
// and skips lazy-alloc, pushing directly into the preserved buffer).
//
// For a first-ever compute, slot->deps is NULL — record_dep_on_parent
// lazy-allocs on first dep recording, same as before.
static void query_stack_push(struct db *s, QueryKind kind, const void *key,
                             Vec *inherited_deps) {
  QueryFrame frame = {
      .kind = kind,
      .key = key,
      .min_input_dur = DUR_HIGH, // lowered by deps / noted inputs
      .dur_set = false,
      .deps = inherited_deps,
#ifdef ORE_DEBUG_QUERIES
      .has_untracked_read = false,
      .untracked_read_count = 0,
#endif
  };
  vec_push(&s->query_stack, &frame);
}

static void query_stack_pop(struct db *s) {
  if (s->query_stack.count > 0) {
    s->query_stack.count--;
  }
}

QueryFrame *db_query_stack_top(struct db *s) {
  if (s->query_stack.count == 0)
    return NULL;
  return (QueryFrame *)vec_get(&s->query_stack, s->query_stack.count - 1);
}

QueryBeginResult db_query_begin(struct db *s, QueryKind kind, const void *key) {
  if (db_check_cancel(s)) {
    return QUERY_BEGIN_CANCELED;
  }

#ifdef ORE_DEBUG_QUERIES
  s->query_stats[(int)kind].begin++;
#endif

  QuerySlot *slot = db_locate_slot(s, kind, key);
  assert(slot != NULL &&
         "db_query_begin: db_locate_slot returned NULL — slot kind not wired");

  uint64_t eff_rev = db_effective_revision(s);
  slot->last_accessed_rev = eff_rev;

  switch (slot->state) {
  case QUERY_DONE: {
    if (db_invalidation_enabled(s) &&
        db_revalidate(s, slot) == DB_REVALIDATE_RECOMPUTE) {
      slot->last_fingerprint = slot->fingerprint;
      slot->state = QUERY_EMPTY;
      slot->fingerprint = FINGERPRINT_NONE;
      if (slot->deps) {
        vec_clear(slot->deps);
      }
      slot->has_untracked_read = false;
      goto compute;
    }
    record_dep_on_parent(s, kind, key, slot->fingerprint, 0);
#ifdef ORE_DEBUG_QUERIES
    s->query_stats[(int)kind].cached_hit++;
#endif
    return QUERY_BEGIN_CACHED;
  }
  case QUERY_ERROR: {
    if (db_invalidation_enabled(s) &&
        db_revalidate(s, slot) == DB_REVALIDATE_RECOMPUTE) {
      slot->last_fingerprint = slot->fingerprint;
      slot->state = QUERY_EMPTY;
      slot->fingerprint = FINGERPRINT_NONE;
      if (slot->deps) {
        vec_clear(slot->deps);
      }
      slot->has_untracked_read = false;
      goto compute;
    }
    record_dep_on_parent(s, kind, key, slot->fingerprint, 0);
#ifdef ORE_DEBUG_QUERIES
    s->query_stats[(int)kind].cached_hit++;
#endif
    return QUERY_BEGIN_ERROR;
  }
  case QUERY_RUNNING:
#ifdef ORE_DEBUG_QUERIES
    s->query_stats[(int)kind].cycle++;
#endif
    return QUERY_BEGIN_CYCLE;
  case QUERY_EMPTY:
    break;
  }

compute:
  if (slot->diag_arena) {
    arena_reset(slot->diag_arena);
    slot->diags = NULL;
    slot->diag_error_count = 0;
  }

  slot->state = QUERY_RUNNING;
  slot->kind = kind;
  query_stack_push(s, kind, key, slot->deps);
  return QUERY_BEGIN_COMPUTE;
}

void db_query_succeed(struct db *s, QueryKind kind, const void *key,
                      Fingerprint fp) {
  QuerySlot *slot = db_locate_slot(s, kind, key);
  assert(slot != NULL && "db_query_succeed: db_locate_slot returned NULL");

  QueryFrame *top = db_query_stack_top(s);
  assert(top != NULL && "db_query_succeed: query stack is empty");
  assert(top->kind == kind && top->key == key &&
         "db_query_succeed: top of stack does not match (kind, key)");

  slot->state = QUERY_DONE;
  slot->fingerprint = fp;
  slot->computed_rev = db_current_revision(s);
  slot->verified_rev = db_current_revision(s);

  bool value_changed = (slot->fingerprint != slot->last_fingerprint);
  if (value_changed) {
    slot->changed_rev = db_current_revision(s);
  }

  // Adopt the frame's deps Vec. Same pointer as slot->deps in the
  // recompute case (because we transferred it in begin); a freshly
  // arena-allocated Vec in the first-ever-compute case (allocated
  // by record_dep_on_parent during the body).
  slot->deps = top->deps;
#ifdef ORE_DEBUG_QUERIES
  slot->has_untracked_read = top->has_untracked_read;
#else
  slot->has_untracked_read = false;
#endif

  // Durability = MIN over inputs (deps + noted untracked inputs). If
  // the body recorded neither, it's an undeclared input query — pin to
  // DUR_LOW so the durability fast-path can never wrongly skip it.
  slot->durability = top->dur_set ? top->min_input_dur : DUR_LOW;

  record_dep_on_parent(s, kind, key, fp, 1);

#ifdef ORE_DEBUG_QUERIES
  s->query_stats[(int)slot->kind].compute++;
  if (value_changed)
    s->query_stats[(int)slot->kind].compute_value_changed++;
  else
    s->query_stats[(int)slot->kind].compute_value_stable++;
#endif

  query_stack_pop(s);
}

void db_query_fail(struct db *s, QueryKind kind, const void *key) {
  QuerySlot *slot = db_locate_slot(s, kind, key);
  assert(slot != NULL && "db_query_fail: db_locate_slot returned NULL");

  QueryFrame *top = db_query_stack_top(s);
  assert(top != NULL && "db_query_fail: query stack is empty");
  assert(top->kind == kind && top->key == key &&
         "db_query_fail: top of stack does not match (kind, key)");

  slot->state = QUERY_ERROR;
  slot->verified_rev = db_current_revision(s);

  slot->deps = top->deps;
#ifdef ORE_DEBUG_QUERIES
  slot->has_untracked_read = top->has_untracked_read;
#else
  slot->has_untracked_read = false;
#endif
  slot->durability = top->dur_set ? top->min_input_dur : DUR_LOW;

#ifdef ORE_DEBUG_QUERIES
  s->query_stats[(int)slot->kind].error++;
#endif

  query_stack_pop(s);
}

void db_query_note_input_durability(struct db *s, uint8_t dur) {
  QueryFrame *top = db_query_stack_top(s);
  if (!top)
    return;
  if (!top->dur_set || dur < top->min_input_dur)
    top->min_input_dur = dur;
  top->dur_set = true;
}

uint64_t db_input_changed(struct db *s, uint8_t dur) {
  // Bump the global current revision (CAS only the CURRENT bits, like
  // rev_set_request does for the REQUEST bits) and stamp every tier at
  // least as volatile as `dur` (i <= dur) as changed at it — the Salsa
  // report_tracked_write rule. A LOW edit bumps only [LOW]; a HIGH
  // edit bumps [LOW..HIGH], so a HIGH-only slot survives LOW edits.
  uint64_t old = atomic_load(&s->rev_control);
  uint64_t newcur, new_val;
  do {
    newcur = ((old & REV_CURRENT_MASK) >> 32) + 1;
    new_val = (old & ~REV_CURRENT_MASK) | ((newcur << 32) & REV_CURRENT_MASK);
  } while (!atomic_compare_exchange_weak(&s->rev_control, &old, new_val));

  if (dur >= DUR_COUNT)
    dur = DUR_COUNT - 1;
  for (uint8_t i = 0; i <= dur; i++)
    atomic_store(&s->dur_last_changed[i], newcur);
  return newcur;
}

const char *db_query_kind_str(QueryKind kind) {
  switch (kind) {
  case QUERY_TYPE_OF_DECL:
    return "type_of_decl";
  case QUERY_LAYOUT_OF_TYPE:
    return "layout_of_type";
  case QUERY_INSTANTIATE_DECL:
    return "instantiate_decl";
  case QUERY_EFFECT_SIG:
    return "effect_sig";
  case QUERY_BODY_EFFECTS:
    return "body_effects";
  case QUERY_MODULE_AST:
    return "module_ast";
  case QUERY_FILE_AST:
    return "file_ast";
  case QUERY_MODULE_EXPORTS:
    return "module_exports";
  case QUERY_MODULE_FOR_PATH:
    return "module_for_path";
  case QUERY_TOP_LEVEL_INDEX:
    return "top_level_index";
  case QUERY_SCOPE_FOR_NODE:
    return "scope_for_node";
  case QUERY_SCOPE_DECLS:
    return "scope_decls";
  case QUERY_SCOPE_PARENT:
    return "scope_parent";
  case QUERY_EFFECT_OPS_VISIBLE:
    return "effect_ops_visible";
  case QUERY_RESOLVE_REF:
    return "resolve_ref";
  case QUERY_RESOLVE_PATH:
    return "resolve_path";
  case QUERY_DEF_IDENTITY:
    return "def_identity";
  case QUERY_NODE_TO_DECL:
    return "node_to_decl";
  case QUERY_FN_SCOPE_INDEX:
    return "fn_scope_index";
  case QUERY_CONST_EVAL:
    return "const_eval";
  case QUERY_INFER_BODY:
    return "infer_body";
  case QUERY_FN_SIGNATURE:
    return "fn_signature";
  case QUERY_STRUCT_SIGNATURE:
    return "struct_signature";
  case QUERY_ENUM_SIGNATURE:
    return "enum_signature";
  case QUERY_IS_COMPTIME:
    return "is_comptime";
  case QUERY_BODY_STORE:
    return "body_store";
  }
  return "query";
}

#ifdef ORE_DEBUG_QUERIES
void db_mark_frame_untracked(struct db *s, const char *why) {
  (void)why;
  QueryFrame *top = db_query_stack_top(s);
  if (!top)
    return;
  top->has_untracked_read = true;
  top->untracked_read_count++;
}

void db_dump_query_stats(struct db *s, FILE *out) {
  if (!s || !out)
    return;
  fprintf(out, "%-22s %8s %8s %8s %8s %8s %6s %6s %8s\n", "kind", "begin",
          "cached", "compute", "changed", "stable", "cycle", "error",
          "untracked");
  fprintf(out, "%-22s %8s %8s %8s %8s %8s %6s %6s %8s\n", "----", "-----",
          "------", "-------", "-------", "------", "-----", "-----",
          "---------");
  uint64_t totals[8] = {0};
  for (int k = 0; k < QUERY_KIND_COUNT; k++) {
    struct QueryStats st = s->query_stats[k];
    if (st.begin == 0 && st.compute == 0 && st.error == 0)
      continue;
    fprintf(out, "%-22s %8llu %8llu %8llu %8llu %8llu %6llu %6llu %8llu\n",
            db_query_kind_str((QueryKind)k), (unsigned long long)st.begin,
            (unsigned long long)st.cached_hit, (unsigned long long)st.compute,
            (unsigned long long)st.compute_value_changed,
            (unsigned long long)st.compute_value_stable,
            (unsigned long long)st.cycle, (unsigned long long)st.error,
            (unsigned long long)st.recompute_due_to_untracked);
    totals[0] += st.begin;
    totals[1] += st.cached_hit;
    totals[2] += st.compute;
    totals[3] += st.compute_value_changed;
    totals[4] += st.compute_value_stable;
    totals[5] += st.cycle;
    totals[6] += st.error;
    totals[7] += st.recompute_due_to_untracked;
  }
  fprintf(out, "%-22s %8s %8s %8s %8s %8s %6s %6s %8s\n", "----", "-----",
          "------", "-------", "-------", "------", "-----", "-----",
          "---------");
  fprintf(out, "%-22s %8llu %8llu %8llu %8llu %8llu %6llu %6llu %8llu\n",
          "TOTAL", (unsigned long long)totals[0], (unsigned long long)totals[1],
          (unsigned long long)totals[2], (unsigned long long)totals[3],
          (unsigned long long)totals[4], (unsigned long long)totals[5],
          (unsigned long long)totals[6], (unsigned long long)totals[7]);
}
#endif
