#ifndef ORE_SEMA_CONST_EVAL_H
#define ORE_SEMA_CONST_EVAL_H

#include <stdbool.h>
#include <stdint.h>

#include "../common/vec.h"

struct Sema;
struct Expr;
struct Type;
struct Decl;

typedef enum {
    CONST_INVALID,        // could not be evaluated at compile time
    CONST_INT,
    CONST_FLOAT,
    CONST_BOOL,
    CONST_TYPE,
    CONST_STRING,
} ConstValueKind;

struct ConstValue {
    ConstValueKind kind;
    union {
        int64_t int_val;        // CONST_INT (also used for usize / isize / sizeof results)
        double float_val;       // CONST_FLOAT
        bool bool_val;          // CONST_BOOL
        struct Type* type_val;  // CONST_TYPE
        uint32_t string_id;     // CONST_STRING (interned in the string pool)
    };
};

// Binding from a Decl* to a ConstValue. ComptimeEnv chains frames so a callee's
// substitutions shadow the caller's without copying.
struct ComptimeCell {
    struct ConstValue value;
};

struct ComptimeBinding {
    struct Decl* decl;
    struct ComptimeCell* cell;
};


// Comptime Environment
struct ComptimeEnv {
    Vec* bindings;                  // Vec of ComptimeBinding
    struct ComptimeEnv* parent;     // outer scope (caller / module)
};

struct ComptimeEnv* sema_comptime_env_new(struct Sema* sema, struct ComptimeEnv* parent);
void sema_comptime_env_bind(struct Sema* sema, struct ComptimeEnv* env,
    struct Decl* decl, struct ConstValue value);
bool sema_comptime_env_lookup(struct ComptimeEnv* env, struct Decl* decl,
    struct ConstValue* out);

struct ConstValue sema_const_invalid(void);
struct ConstValue sema_const_int(int64_t value);
struct ConstValue sema_const_float(double value);
struct ConstValue sema_const_bool(bool value);
struct ConstValue sema_const_type(struct Type* type);
struct ConstValue sema_const_string(uint32_t string_id);

bool sema_const_value_is_valid(struct ConstValue value);
bool sema_const_value_equal(struct ConstValue a, struct ConstValue b);

struct ConstValue sema_const_eval_expr(struct Sema* sema, struct Expr* expr,
    struct ComptimeEnv* env);

void sema_comptime_env_assign(struct Sema* sema, struct ComptimeEnv* env, struct Decl* decl, struct ConstValue value);

#endif // ORE_SEMA_CONST_EVAL_H
