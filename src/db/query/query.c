#include "query.h"

#include <assert.h>

#include "../db.h"
#include "../diag/diag.h"
#include "../request/request.h"
#include "invalidate.h"

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
                                 uint64_t child_key, Fingerprint child_fp,
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
  QuerySlotHot *child = db_locate_slot(s, child_kind, child_key);
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
static void query_stack_push(struct db *s, QueryKind kind, uint64_t key,
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

// Record a slot that just transitioned to QUERY_RUNNING. db_request_end
// sweeps the list and resets any leftover RUNNING (defensive — covers
// paths that exit without reaching db_query_succeed/fail).
static void track_running_slot(struct db *s, QueryKind kind, uint64_t key) {
  QueryRunningRef ref = {.kind = kind, .key = key};
  vec_push(&s->running_slots, &ref);
}

QueryFrame *db_query_stack_top(struct db *s) {
  if (s->query_stack.count == 0)
    return NULL;
  return (QueryFrame *)vec_get(&s->query_stack, s->query_stack.count - 1);
}

QueryBeginResult db_query_begin(struct db *s, QueryKind kind, uint64_t key) {
  if (db_check_cancel(s)) {
    return QUERY_BEGIN_CANCELED;
  }

#ifdef ORE_DEBUG_QUERIES
  s->query_stats[(int)kind].begin++;
#endif

  QuerySlotHot *slot = db_locate_slot(s, kind, key);
  assert(slot != NULL &&
         "db_query_begin: db_locate_slot returned NULL — slot kind not wired");

  switch (slot->state) {
  case QUERY_DONE:
  case QUERY_ERROR: {
    QueryState prev = slot->state;
    QueryBeginResult cached_result =
        (prev == QUERY_DONE) ? QUERY_BEGIN_CACHED : QUERY_BEGIN_ERROR;

    // Invalidation disabled — short-circuit to the cached value.
    if (!db_invalidation_enabled(s)) {
      record_dep_on_parent(s, kind, key, slot->fingerprint, 0);
#ifdef ORE_DEBUG_QUERIES
      s->query_stats[(int)kind].cached_hit++;
#endif
      return cached_result;
    }

    // Push P's frame BEFORE verify so verify-driven dep pulls record
    // onto P itself (dedup against P's existing deps). RUNNING is a
    // unified marker — recursive begin during verify hits the cycle
    // case below, same as it does during compute.
    slot->kind = kind;
    slot->state = QUERY_RUNNING;
    query_stack_push(s, kind, key, slot->deps);

    if (db_verify(s, slot)) {
      // Cached: pop frame, restore prior state, record onto caller.
      slot->state = prev;
      slot->verified_rev = db_effective_revision(s);
      record_dep_on_parent(s, kind, key, slot->fingerprint, 1);
      query_stack_pop(s);
#ifdef ORE_DEBUG_QUERIES
      s->query_stats[(int)kind].cached_hit++;
#endif
      return cached_result;
    }

    // Verify failed: a dep value changed. Reset for recompute; the
    // frame stays on the stack and the body will run in it. State
    // stays RUNNING — the recompute is just the body executing.
    QueryFrame *top = db_query_stack_top(s);
    if (top && top->deps) {
      vec_clear(top->deps);
    }
    slot->fingerprint = FINGERPRINT_NONE;
    slot->has_untracked_read = false;
    db_diags_clear(s, kind, key); // drop prior-run diagnostics
    track_running_slot(s, kind, key);
    return QUERY_BEGIN_COMPUTE;
  }
  case QUERY_RUNNING:
#ifdef ORE_DEBUG_QUERIES
    s->query_stats[(int)kind].cycle++;
#endif
    return QUERY_BEGIN_CYCLE;
  case QUERY_EMPTY:
    // First-ever compute (or post-input-stale). No prior diagnostics
    // to clear (slot has never run).
    slot->state = QUERY_RUNNING;
    slot->kind = kind;
    query_stack_push(s, kind, key, slot->deps);
    track_running_slot(s, kind, key);
    return QUERY_BEGIN_COMPUTE;
  }

  // Unreachable: switch above is exhaustive over QueryState.
  return QUERY_BEGIN_CYCLE;
}

void db_query_succeed(struct db *s, QueryKind kind, uint64_t key,
                      Fingerprint fp) {
  QuerySlotHot *slot = db_locate_slot(s, kind, key);
  QuerySlotCold *cold = db_locate_slot_cold(s, kind, key);
  assert(slot != NULL && cold != NULL &&
         "db_query_succeed: db_locate_slot returned NULL");

  QueryFrame *top = db_query_stack_top(s);
  assert(top != NULL && "db_query_succeed: query stack is empty");
  assert(top->kind == kind && top->key == key &&
         "db_query_succeed: top of stack does not match (kind, key)");

  uint64_t cur = db_current_revision(s);
  slot->state = QUERY_DONE;
  slot->fingerprint = fp;
  slot->verified_rev = cur;
  cold->computed_rev = cur;

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
#endif

  query_stack_pop(s);
}

void db_query_fail(struct db *s, QueryKind kind, uint64_t key) {
  QuerySlotHot *slot = db_locate_slot(s, kind, key);
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
  case QUERY_NAMESPACE_SCOPES:
    return "module_exports";
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
  case QUERY_BODY_SCOPES:
    return "body_scopes";
  case QUERY_DECL_AST:
    return "decl_ast";
  case QUERY_FILE_IMPORTS:
    return "file_imports";
  case QUERY_NAMESPACE_TYPE:
    return "namespace_type";
  case QUERY_UNUSED_WARNINGS:
    return "unused_warnings";
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
  fprintf(out, "%-22s %8s %8s %8s %6s %6s %8s\n", "kind", "begin", "cached",
          "compute", "cycle", "error", "untracked");
  fprintf(out, "%-22s %8s %8s %8s %6s %6s %8s\n", "----", "-----", "------",
          "-------", "-----", "-----", "---------");
  uint64_t totals[6] = {0};
  for (int k = 0; k < QUERY_KIND_COUNT; k++) {
    struct QueryStats st = s->query_stats[k];
    if (st.begin == 0 && st.compute == 0 && st.error == 0)
      continue;
    fprintf(out, "%-22s %8llu %8llu %8llu %6llu %6llu %8llu\n",
            db_query_kind_str((QueryKind)k), (unsigned long long)st.begin,
            (unsigned long long)st.cached_hit, (unsigned long long)st.compute,
            (unsigned long long)st.cycle, (unsigned long long)st.error,
            (unsigned long long)st.recompute_due_to_untracked);
    totals[0] += st.begin;
    totals[1] += st.cached_hit;
    totals[2] += st.compute;
    totals[3] += st.cycle;
    totals[4] += st.error;
    totals[5] += st.recompute_due_to_untracked;
  }
  fprintf(out, "%-22s %8s %8s %8s %6s %6s %8s\n", "----", "-----", "------",
          "-------", "-----", "-----", "---------");
  fprintf(out, "%-22s %8llu %8llu %8llu %6llu %6llu %8llu\n", "TOTAL",
          (unsigned long long)totals[0], (unsigned long long)totals[1],
          (unsigned long long)totals[2], (unsigned long long)totals[3],
          (unsigned long long)totals[4], (unsigned long long)totals[5]);
}
#endif
