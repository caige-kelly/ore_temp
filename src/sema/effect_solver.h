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

// Phase E post-pass: walk every function decl in every loaded module
// and verify the body's inferred effect set matches the declared
// signature row. Replaces the per-decl check that used to live in
// `compute_decl_signature` — runs after `sema_check_expressions` (so
// body facts are populated) and after `sema_lower_modules` (so HIR is
// available for the upcoming Phase E.3 switch).
void sema_verify_body_effects(struct Sema* sema);

#endif // ORE_SEMA_EFFECT_SOLVER_H
