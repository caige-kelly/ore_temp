#ifndef ORE_SEMA_TYPE_H
#define ORE_SEMA_TYPE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "../../common/vec.h"
#include "../scope/scope.h"
#include "layout.h"
#include "../query/query.h"

struct EffectSig;
struct Sema;

typedef enum {
    TYPE_UNKNOWN,
    TYPE_ERROR,
    TYPE_VOID,
    TYPE_NORETURN,    // bottom type — assignable to any type, no values inhabit it
    TYPE_BOOL,
    TYPE_COMPTIME_INT,
    TYPE_COMPTIME_FLOAT,
    TYPE_U8,
    TYPE_U16,
    TYPE_U32,
    TYPE_U64,
    TYPE_USIZE,
    TYPE_I8,
    TYPE_I16,
    TYPE_I32,
    TYPE_I64,
    TYPE_ISIZE,
    TYPE_F64,
    TYPE_F32,
    TYPE_STRING,
    TYPE_NIL,
    TYPE_TYPE,
    TYPE_ANYTYPE,
    TYPE_MODULE,
    TYPE_STRUCT,
    TYPE_ENUM,
    TYPE_EFFECT,
    TYPE_EFFECT_ROW,
    TYPE_SCOPE_TOKEN,
    TYPE_FUNCTION,
    TYPE_HANDLER,    // HandlerOf<E, R> — value form of `handler { ops }`
    TYPE_POINTER,
    TYPE_SLICE,
    TYPE_ARRAY,
    TYPE_PRODUCT,
} TypeKind;

struct Type {
    TypeKind kind;
    uint32_t name_id;
    struct Decl* decl;
    struct Type* elem;
    struct Type* ret;
    Vec* params;
    Vec* effects;
    struct EffectSig* effect_sig;
    uint32_t region_id;
    struct QuerySlot layout_query;
    struct TypeLayout layout;
    // `?T`. Independent of TypeKind so any type can be optional. nil flows
    // into any optional; an optional must be unwrapped (via `if (opt) |x|`,
    // `orelse`, or comparison-to-nil) before its inner shape is usable.
    bool is_optional;
    // `const T`. Set on the *pointee* of `^const T` and the *element* of
    // `[]const T` / `[N]const T`. Marks "l-values reached through this Type
    // are not writable." Mirrors `is_optional`'s per-Type-flag pattern;
    // threaded through every constructor / equality / assignability /
    // display path. Applies to bare value types too (idempotent — a value
    // of type `const i32` has no l-value path so the flag is harmless until
    // wrapped by a pointer/slice).
    bool is_const;
    int64_t array_length;
};

struct Type* sema_type_new(struct Sema* sema, TypeKind kind);
struct Type* sema_named_type(struct Sema* sema, TypeKind kind, uint32_t name_id, struct Decl* decl);
struct Type* sema_pointer_type(struct Sema* sema, struct Type* elem);
struct Type* sema_slice_type(struct Sema* sema, struct Type* elem);
struct Type* sema_array_type(struct Sema* sema, struct Type* elem);
struct Type* sema_function_type(struct Sema* sema);
// HandlerOf<E, R>: value type of an `expr_Handler`. `effect_decl` is the
// effect E this handler discharges; `ret` is R, the return type observed
// by the handled action. R may be NULL when the handler has no `return`
// clause and the action's R is unconstrained at the value-creation site
// — equality treats NULL R as a wildcard so a handler without `return`
// unifies against any R the call site demands.
struct Type* sema_handler_type(struct Sema* sema, struct Decl* effect_decl, struct Type* ret);
// Wrap a type as optional. If `inner` is already optional, returns it
// unchanged so `??T` is still `?T`.
struct Type* sema_optional_type(struct Sema* sema, struct Type* inner);
// Wrap a type as const-qualified. If `inner` is already const, returns it
// unchanged so `const const T` is still `const T`. Used by `unary_Const`
// in type position to attach the read-only marker — mutation gates
// downstream reject writes through l-values whose type carries this bit.
// Named with a `_qualified_` infix to avoid collision with const_eval's
// `sema_const_type` (which constructs a ConstValue, not a Type).
struct Type* sema_const_qualified_type(struct Sema* sema, struct Type* inner);
// Strip a single layer of optionality, returning a non-optional view of
// the same shape. If `type` isn't optional, returns it unchanged. Used by
// unwrap forms (`if (opt) |x|`, `orelse`) to bind the unwrapped value.
struct Type* sema_unwrap_optional(struct Sema* sema, struct Type* type);
struct Type* sema_primitive_type_for_name(struct Sema* sema, uint32_t id);
SemanticKind sema_semantic_for_type(struct Type* type);
const char* sema_type_kind_str(TypeKind kind);
const char* sema_semantic_kind_str(SemanticKind kind);
const char* sema_type_display_name(struct Sema* sema, struct Type* type, char* buffer, size_t buffer_size);
bool sema_name_is(struct Sema* sema, uint32_t id, const char* name);
bool sema_type_is_errorish(struct Type* type);
bool sema_type_is_nominal(struct Type* type);
struct Type* sema_numeric_join(struct Sema* s, struct Type* a, struct Type* b);
bool sema_type_is_type_value(struct Type* type);
bool sema_type_is_numeric(struct Type* type);
bool sema_type_is_float(struct Type* type);
bool sema_type_is_integer(struct Type* type);
bool sema_type_is_callable(struct Type* type);
bool sema_type_equal(struct Type* left, struct Type* right);
bool sema_type_assignable(struct Type* expected, struct Type* actual);

#endif // ORE_SEMA_TYPE_H