#ifndef SEMA_QUERY_H
#define SEMA_QUERY_H

#include <stdbool.h>

#include "../lexer/token.h"

struct Sema;

typedef enum {
    QUERY_EMPTY,
    QUERY_RUNNING,
    QUERY_DONE,
    QUERY_ERROR,
} QueryState;

typedef enum {
    QUERY_TYPE_OF_DECL,
    QUERY_SIGNATURE_OF_DECL,
    QUERY_LAYOUT_OF_TYPE,
    QUERY_CONST_EVAL_EXPR,
    QUERY_INSTANTIATE_DECL,
    QUERY_EFFECT_SIG,
    QUERY_BODY_EFFECTS,
    QUERY_EVIDENCE_AT,
    QUERY_EVIDENCE_OF_BODY,
} QueryKind;

typedef enum {
    QUERY_BEGIN_COMPUTE,
    QUERY_BEGIN_CACHED,
    QUERY_BEGIN_CYCLE,
    QUERY_BEGIN_ERROR,
} QueryBeginResult;

struct QuerySlot {
    QueryState state;
    QueryKind kind;
    bool cycle_reported;
};

struct QueryFrame {
    QueryKind kind;
    const void* key;
    struct Span span;
};

void sema_query_slot_init(struct QuerySlot* slot, QueryKind kind);
QueryBeginResult sema_query_begin(struct Sema* sema, struct QuerySlot* slot,
    QueryKind kind, const void* key, struct Span span);
void sema_query_succeed(struct Sema* sema, struct QuerySlot* slot);
void sema_query_fail(struct Sema* sema, struct QuerySlot* slot);
const char* sema_query_kind_str(QueryKind kind);

#endif // SEMA_QUERY_H
