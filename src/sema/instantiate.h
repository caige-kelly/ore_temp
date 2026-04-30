#ifndef ORE_SEMA_INSTANTIATE_H
#define ORE_SEMA_INSTANTIATE_H

#include <stdbool.h>
#include <stddef.h>

#include "../common/vec.h"
#include "const_eval.h"
#include "query.h"

struct Sema;
struct Decl;
struct Type;
struct EffectSig;
struct CheckedBody;
struct Param;
struct Expr;

// Bound comptime arguments for one instantiation of a generic decl. The values
// vector mirrors the generic decl's comptime parameters in declaration order.
struct ComptimeArgTuple {
    Vec* values;  // Vec of ConstValue
};

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
