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

#endif // ORE_SEMA_EFFECT_SOLVER_H
