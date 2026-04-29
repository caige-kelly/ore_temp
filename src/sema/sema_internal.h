#ifndef ORE_SEMA_INTERNAL_H
#define ORE_SEMA_INTERNAL_H

#include "sema.h"

void sema_error(struct Sema* sema, struct Span span, const char* fmt, ...);
void sema_record_fact(struct Sema* sema, struct Expr* expr, struct Type* type,
    SemanticKind semantic_kind, uint32_t region_id);

struct CheckedBody* sema_body_new(struct Sema* sema, struct Decl* decl,
    struct Module* module, struct Instantiation* instantiation);
struct CheckedBody* sema_enter_body(struct Sema* sema, struct CheckedBody* body);
void sema_leave_body(struct Sema* sema, struct CheckedBody* previous);

#endif // ORE_SEMA_INTERNAL_H