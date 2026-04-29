#ifndef ORE_SEMA_TYPE_H
#define ORE_SEMA_TYPE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "../common/vec.h"
#include "../name_resolution/name_resolution.h"
#include "layout.h"
#include "query.h"

struct EffectSig;
struct Sema;

typedef enum {
    TYPE_UNKNOWN,
    TYPE_ERROR,
    TYPE_VOID,
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
};

struct Type* sema_type_new(struct Sema* sema, TypeKind kind);
struct Type* sema_named_type(struct Sema* sema, TypeKind kind, uint32_t name_id, struct Decl* decl);
struct Type* sema_pointer_type(struct Sema* sema, struct Type* elem);
struct Type* sema_slice_type(struct Sema* sema, struct Type* elem);
struct Type* sema_array_type(struct Sema* sema, struct Type* elem);
struct Type* sema_function_type(struct Sema* sema);
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