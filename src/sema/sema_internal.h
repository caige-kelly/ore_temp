#ifndef ORE_SEMA_INTERNAL_H
#define ORE_SEMA_INTERNAL_H

#include "sema.h"

void sema_error(struct Sema* sema, struct Span span, const char* fmt, ...);

// Canonical fact-recording entry point. Writes the fact into the supplied
// CheckedBody's facts vec. Callers that have a body in hand should prefer this.
void body_record_fact(struct Sema* sema, struct CheckedBody* body, struct Expr* expr,
    struct Type* type, SemanticKind semantic_kind, uint32_t region_id);

// Convenience wrapper that uses sema->current_body. If current_body is NULL
// the call is a no-op; the warning hook in sema.c flags it once at startup so
// missing-body bugs surface instead of silently dropping facts.
void sema_record_fact(struct Sema* sema, struct Expr* expr, struct Type* type,
    SemanticKind semantic_kind, uint32_t region_id);

struct CheckedBody* sema_body_new(struct Sema* sema, struct Decl* decl,
    struct Module* module, struct Instantiation* instantiation);
struct CheckedBody* sema_enter_body(struct Sema* sema, struct CheckedBody* body);
void sema_leave_body(struct Sema* sema, struct CheckedBody* previous);

#endif // ORE_SEMA_INTERNAL_H