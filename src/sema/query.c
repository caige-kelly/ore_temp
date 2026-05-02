#include "query.h"

#include "sema.h"

void sema_query_slot_init(struct QuerySlot* slot, QueryKind kind) {
    if (!slot) return;
    slot->state = QUERY_EMPTY;
    slot->kind = kind;
}

static void query_stack_push(struct Sema* s, QueryKind kind, const void* key,
    struct Span span) {
    if (!s || !s->query_stack) return;
    struct QueryFrame frame = {
        .kind = kind,
        .key = key,
        .span = span,
    };
    vec_push(s->query_stack, &frame);
}

static void query_stack_pop(struct Sema* s) {
    if (!s || !s->query_stack || s->query_stack->count == 0) return;
    s->query_stack->count--;
}

QueryBeginResult sema_query_begin(struct Sema* s, struct QuerySlot* slot,
    QueryKind kind, const void* key, struct Span span) {
    if (!slot) return QUERY_BEGIN_ERROR;

    switch (slot->state) {
        case QUERY_DONE:
            return QUERY_BEGIN_CACHED;
        case QUERY_ERROR:
            return QUERY_BEGIN_ERROR;
        case QUERY_RUNNING:
            return QUERY_BEGIN_CYCLE;
        case QUERY_EMPTY:
            break;
    }

    slot->state = QUERY_RUNNING;
    slot->kind = kind;
    query_stack_push(s, kind, key, span);
    return QUERY_BEGIN_COMPUTE;
}

void sema_query_succeed(struct Sema* s, struct QuerySlot* slot) {
    if (!slot) return;
    if (slot->state != QUERY_ERROR) {
        slot->state = QUERY_DONE;
    }
    query_stack_pop(s);
}

void sema_query_fail(struct Sema* s, struct QuerySlot* slot) {
    if (!slot) return;
    slot->state = QUERY_ERROR;
    query_stack_pop(s);
}

const char* sema_query_kind_str(QueryKind kind) {
    switch (kind) {
        case QUERY_TYPE_OF_DECL:      return "type_of_decl";
        case QUERY_LAYOUT_OF_TYPE:    return "layout_of_type";
        case QUERY_INSTANTIATE_DECL:  return "instantiate_decl";
        case QUERY_EFFECT_SIG:        return "effect_sig";
        case QUERY_BODY_EFFECTS:      return "body_effects";
    }
    return "query";
}
