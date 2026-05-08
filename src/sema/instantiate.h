#ifndef ORE_SEMA_INSTANTIATE_H
#define ORE_SEMA_INSTANTIATE_H

#include <stdbool.h>
#include <stddef.h>

#include "../common/vec.h"
#include "const_eval.h"
#include "query/query.h"
#include "sema_internal.h"

struct Sema;
struct Decl;
struct Type;
struct EffectSig;
struct CheckedBody;
struct HirFn;
struct Param;
struct Expr;


// One instantiation of a generic decl. Owns its specialized type, effect
// signature, and CheckedBody, so multiple call sites with different comptime
// args produce distinct facts.
struct Instantiation {
    struct Decl* generic;
    struct ComptimeArgTuple args;
    struct Type* specialized_type;
    struct EffectSig* specialized_sig;
    struct CheckedBody* body;
    struct ComptimeEnv* env;
    struct QuerySlot query;
    // Per-instantiation HIR — built during sema_lower_modules' second
    // pass alongside the per-instantiation CheckedBody facts. Each
    // instantiation gets its own HirFn because comptime args produce
    // structurally different lowered shapes (different types in
    // calls, different folded values, different specialized effect
    // rows). Phase G's per-instantiation effect verification walks
    // this HirFn instead of re-walking the generic AST body.
    struct HirFn* hir;
};

bool sema_param_is_comptime(struct Param* param);     // any non-runtime kind
bool sema_param_is_inferred(struct Param* param);     // INFERRED_COMPTIME only
bool sema_decl_is_generic(struct Decl* decl);
size_t sema_decl_comptime_param_count(struct Decl* decl);
size_t sema_param_visible_arity(Vec* params);          // RUNTIME + COMPTIME (caller-supplied)
Vec* sema_decl_function_params(struct Decl* decl);
struct Expr* sema_decl_function_body(struct Decl* decl);
struct Expr* sema_decl_function_ret_type_expr(struct Decl* decl);

struct ComptimeArgTuple sema_arg_tuple_new(struct Sema* sema);
void sema_arg_tuple_push(struct ComptimeArgTuple* tuple, struct ConstValue value);
bool sema_arg_tuple_equal(const struct ComptimeArgTuple* a, const struct ComptimeArgTuple* b);

struct Instantiation* sema_instantiate_decl(struct Sema* sema, struct Decl* generic,
    struct ComptimeArgTuple args);

#endif // ORE_SEMA_INSTANTIATE_H
