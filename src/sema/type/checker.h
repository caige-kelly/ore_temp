#ifndef ORE_SEMA_TYPE_CHECKER_H
#define ORE_SEMA_TYPE_CHECKER_H

#include "../ids/ids.h"

struct Sema;
struct Type;
struct Expr;

// Compute the type of any declaration. Slot lives on
// `SemaDeclInfo.type_query` so cycle detection + invalidation flow
// through the standard query machinery.
//
// Behavior, by DeclKind:
//   - DECL_PRIMITIVE  → `type_for_primitive_name(name_id)`
//   - DECL_USER       → resolve type_ann if present (and check value
//                       coerces), else infer from value
//   - DECL_PARAM      → resolve `type_ann_expr` in NS_TYPE
//   - DECL_FIELD      → resolve `type_ann_expr` (E.3 land for full
//                       struct/enum surface)
//   - DECL_IMPORT     → s->module_type (placeholder until module
//                       types get real semantics)
//   - DECL_SCOPE_PARAM/EFFECT_ROW/LOOP_LABEL → s->error_type (these
//                       don't have value-typing semantics)
//
// Returns Sema's `error_type` on any failure; never NULL.
struct Type *query_type_of_def(struct Sema *s, DefId def);

// Resolve a type-position Expr* to a Type*. Handles primitive Idents
// + array/slice/pointer/many-pointer compound type expressions.
// Public so query_type_of_expr can call it for type-position
// children (e.g. fn return type, param annotation).
struct Type *resolve_type_expr(struct Sema *s, struct Expr *e);

#endif
