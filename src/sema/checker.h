#ifndef ORE_SEMA_CHECKER_H
#define ORE_SEMA_CHECKER_H

#include <stdbool.h>

#include "sema.h"

struct Type* sema_infer_expr(struct Sema* sema, struct Expr* expr);
struct Type* sema_infer_type_expr(struct Sema* sema, struct Expr* expr);
bool sema_check_expr(struct Sema* sema, struct Expr* expr, struct Type* expected);
bool sema_check_expressions(struct Sema* sema);

#endif // ORE_SEMA_CHECKER_H