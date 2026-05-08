#include "query.h"

#include "../../common/vec.h"
#include "../sema.h"

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

// Record `child` as a dependency of whatever query is currently running
// (the top of the query stack). Called from within sema_query_begin so
// every child compute is observed by its parent frame. Hooked for
// future incremental — today nothing reads back the recorded deps.
static void record_dep_on_parent(struct Sema *s, QueryKind child_kind,
                                 const void *child_key) {
  if (s->query_stack->count < 2)
    return;
  struct QueryFrame *parent =
      (struct QueryFrame *)vec_get(s->query_stack, s->query_stack->count - 2);
  if (!parent->deps)
    parent->deps = vec_new_in(s->arena, sizeof(struct QueryDep));
  struct QueryDep dep = {.kind = child_kind, .key = child_key};
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
  switch (slot->state) {
  case QUERY_DONE:
    record_dep_on_parent(s, kind, key);
    return QUERY_BEGIN_CACHED;
  case QUERY_ERROR:
    record_dep_on_parent(s, kind, key);
    return QUERY_BEGIN_ERROR;
  case QUERY_RUNNING:
    return QUERY_BEGIN_CYCLE;
  case QUERY_EMPTY:
    break;
  }

  slot->state = QUERY_RUNNING;
  slot->kind = kind;
  query_stack_push(s, slot, kind, key, span);
  // record_dep_on_parent must run after push so the child's frame sits
  // on top and the parent is at top-1.
  record_dep_on_parent(s, kind, key);
  return QUERY_BEGIN_COMPUTE;
}

void sema_query_succeed(struct Sema *s, struct QuerySlot *slot) {
  if (slot->state != QUERY_ERROR) {
    slot->state = QUERY_DONE;
    slot->computed_rev = s->current_revision;
    slot->verified_rev = s->current_revision;
    struct QueryFrame *top = query_stack_top(s);
    if (top && top->slot == slot)
      slot->deps = top->deps;
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
  case QUERY_SCOPE_FOR_NODE:      return "scope_for_node";
  case QUERY_SCOPE_DECLS:         return "scope_decls";
  case QUERY_SCOPE_PARENT:        return "scope_parent";
  case QUERY_EFFECT_OPS_VISIBLE:  return "effect_ops_visible";
  case QUERY_RESOLVE_REF:         return "resolve_ref";
  case QUERY_RESOLVE_PATH:        return "resolve_path";
  case QUERY_CONST_EVAL:          return "const_eval";
  }
  return "query";
}
