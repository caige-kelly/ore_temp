#include "query.h"

#include "../../common/vec.h"
#include "../request/cancel.h"
#include "../request/snapshot.h"
#include "../sema.h"
#include "invalidate.h"

void sema_query_slot_init(struct QuerySlot *slot, QueryKind kind) {
  *slot = (struct QuerySlot){
      .state = QUERY_EMPTY,
      .kind = kind,
      .fingerprint = FINGERPRINT_NONE,
      .computed_rev = 0,
      .verified_rev = 0,
      .deps = NULL,
  };
}

// Record `child` as a dependency of whatever query is currently
// running (the parent frame, at top-1 of the stack). Called from two
// sites:
//   1. sema_query_begin's CACHED return path — the child slot's
//      fingerprint is already stamped, pass it through.
//   2. sema_query_succeed's pre-pop hook — the child has just
//      finished computing, fingerprint is final.
//
// We avoid recording on the COMPUTE branch of begin because the
// fingerprint isn't yet known there.
static void record_dep_on_parent(struct Sema *s, QueryKind child_kind,
                                 const void *child_key, Fingerprint child_fp,
                                 size_t parent_offset) {
  if (s->query_stack->count <= parent_offset)
    return;
  struct QueryFrame *parent = (struct QueryFrame *)vec_get(
      s->query_stack, s->query_stack->count - 1 - parent_offset);
  if (!parent->deps)
    parent->deps = vec_new_in(s->arena, sizeof(struct QueryDep));
  struct QueryDep dep = {
      .kind = child_kind, .key = child_key, .dep_fp = child_fp};
  vec_push(parent->deps, &dep);
}

static void query_stack_push(struct Sema *s, struct QuerySlot *slot,
                             QueryKind kind, const void *key,
                             struct Span span) {
  struct QueryFrame frame = {
      .kind = kind,
      .key = key,
      .span = span,
      .deps = NULL,
      .slot = slot,
  };
  vec_push(s->query_stack, &frame);
}

static struct QueryFrame *query_stack_top(struct Sema *s) {
  if (s->query_stack->count == 0)
    return NULL;
  return (struct QueryFrame *)vec_get(s->query_stack,
                                      s->query_stack->count - 1);
}

static void query_stack_pop(struct Sema *s) {
  if (s->query_stack->count == 0)
    return;
  s->query_stack->count--;
}

QueryBeginResult sema_query_begin(struct Sema *s, struct QuerySlot *slot,
                                  QueryKind kind, const void *key,
                                  struct Span span) {
  // Layer 7.7 — cancellation check. If the active request has
  // been cancelled, every query short-circuits with CANCELED.
  // Callers propagate up the stack and the request handler
  // bails.
  if (sema_check_cancel(s))
    return QUERY_BEGIN_CANCELED;

  // LRU touch — record that this slot is in current use. The
  // future eviction walker reads last_accessed_rev to skip
  // recently-touched slots.
  slot->last_accessed_rev = sema_effective_revision(s);

  switch (slot->state) {
  case QUERY_DONE: {
    // Layer 7.5 — invalidation walker. If the global revision has
    // moved past our verified_rev, walk the dep graph; if any dep
    // changed, transition state to EMPTY and fall through to
    // recompute. Otherwise mark verified at current_revision.
    if (s->invalidation_enabled &&
        sema_revalidate(s, slot) == REVALIDATE_RECOMPUTE) {
      slot->state = QUERY_EMPTY;
      slot->fingerprint = FINGERPRINT_NONE;
      slot->deps = NULL;
      goto compute;
    }
    record_dep_on_parent(s, kind, key, slot->fingerprint, /*parent_offset=*/0);
    return QUERY_BEGIN_CACHED;
  }
  case QUERY_ERROR:
    record_dep_on_parent(s, kind, key, slot->fingerprint, /*parent_offset=*/0);
    return QUERY_BEGIN_ERROR;
  case QUERY_RUNNING:
    return QUERY_BEGIN_CYCLE;
  case QUERY_EMPTY:
    break;
  }
compute:

  slot->state = QUERY_RUNNING;
  slot->kind = kind;
  query_stack_push(s, slot, kind, key, span);
  // No record_dep_on_parent here — fingerprint isn't known yet.
  // sema_query_succeed records the dep at finalization time.
  return QUERY_BEGIN_COMPUTE;
}

void sema_query_succeed(struct Sema *s, struct QuerySlot *slot) {
  if (slot->state != QUERY_ERROR) {
    slot->state = QUERY_DONE;
    slot->computed_rev = s->current_revision;
    slot->verified_rev = s->current_revision;
    struct QueryFrame *top = query_stack_top(s);
    if (top && top->slot == slot) {
      slot->deps = top->deps;
      // Record this completed query onto its parent frame (top-1
      // relative to current stack — since `top` is at offset 0,
      // parent is at offset 1).
      record_dep_on_parent(s, top->kind, top->key, slot->fingerprint,
                           /*parent_offset=*/1);
    }
  }
  query_stack_pop(s);
}

void sema_query_fail(struct Sema *s, struct QuerySlot *slot) {
  slot->state = QUERY_ERROR;
  query_stack_pop(s);
}

const char *sema_query_kind_str(QueryKind kind) {
  switch (kind) {
  case QUERY_TYPE_OF_DECL:        return "type_of_decl";
  case QUERY_LAYOUT_OF_TYPE:      return "layout_of_type";
  case QUERY_INSTANTIATE_DECL:    return "instantiate_decl";
  case QUERY_EFFECT_SIG:          return "effect_sig";
  case QUERY_BODY_EFFECTS:        return "body_effects";
  case QUERY_MODULE_AST:          return "module_ast";
  case QUERY_MODULE_DEF_MAP:      return "module_def_map";
  case QUERY_MODULE_EXPORTS:      return "module_exports";
  case QUERY_MODULE_FOR_PATH:     return "module_for_path";
  case QUERY_TOP_LEVEL_INDEX:     return "top_level_index";
  case QUERY_DEF_FOR_NAME:        return "def_for_name";
  case QUERY_SCOPE_FOR_NODE:      return "scope_for_node";
  case QUERY_SCOPE_DECLS:         return "scope_decls";
  case QUERY_SCOPE_PARENT:        return "scope_parent";
  case QUERY_EFFECT_OPS_VISIBLE:  return "effect_ops_visible";
  case QUERY_RESOLVE_REF:         return "resolve_ref";
  case QUERY_RESOLVE_PATH:        return "resolve_path";
  case QUERY_NODE_TO_DECL:        return "node_to_decl";
  case QUERY_FN_SCOPE_INDEX:      return "fn_scope_index";
  case QUERY_CONST_EVAL:          return "const_eval";
  }
  return "query";
}
