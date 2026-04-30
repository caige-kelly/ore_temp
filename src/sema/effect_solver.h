#ifndef ORE_SEMA_EFFECT_SOLVER_H
#define ORE_SEMA_EFFECT_SOLVER_H

#include <stdbool.h>

#include "../common/vec.h"

struct Sema;
struct Decl;
struct Expr;
struct EffectSig;
struct EffectSet;

struct EffectSig* sema_effect_sig_of_callable(struct Sema* sema, struct Decl* decl);
struct EffectSet* sema_body_effects_of(struct Sema* sema, struct Decl* decl);
bool sema_solve_effect_rows(struct Sema* sema, struct Decl* decl,
    struct EffectSig* declared, struct EffectSet* inferred);

// Walk an arbitrary body expression and return a fresh EffectSet. Unlike
// sema_body_effects_of (which is keyed on a Decl and caches in its query
// slot), this is the caller-owned variant — useful for per-instantiation
// effect checks where each instantiation needs its own set.
struct EffectSet* sema_collect_effects_from_expr(struct Sema* sema, struct Expr* expr);

#endif // ORE_SEMA_EFFECT_SOLVER_H
