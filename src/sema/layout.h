#ifndef ORE_SEMA_LAYOUT_H
#define ORE_SEMA_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

#include "../lexer/token.h"

struct Sema;
struct Type;
struct Expr;

struct TypeLayout {
    uint64_t size;
    uint64_t align;
    bool complete;
};

struct TypeLayout sema_layout_of_type(struct Sema* sema, struct Type* type);
struct TypeLayout sema_layout_of_type_at(struct Sema* sema, struct Type* type,
    struct Span ref_span);

#endif // ORE_SEMA_LAYOUT_H
