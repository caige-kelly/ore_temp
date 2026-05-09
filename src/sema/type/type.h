#ifndef ORE_SEMA_TYPE_H
#define ORE_SEMA_TYPE_H

#include <stdbool.h>
#include <stdint.h>

struct Sema;

// Type representation — Stage E.1 minimum.
//
// Just primitives + comptime numeric placeholders. Complex shapes
// (struct, enum, fn, slice, pointer, effect, etc.) come in later
// stages. Each primitive kind has exactly one canonical Type*
// allocated at sema_init; comparing types is pointer comparison.
//
// `comptime_int` and `comptime_float` are transient — every
// comptime numeric value should either coerce into a concrete int /
// float type at point-of-use or end up dead-coded. They exist as
// real types in the system so range checks can flow naturally,
// but they shouldn't survive past elaboration in any meaningful
// program.

typedef enum {
    TY_ERROR,            // sentinel for "type unknown / error recovery"
    TY_VOID,
    TY_BOOL,
    TY_U8,  TY_U16, TY_U32, TY_U64, TY_USIZE,
    TY_I8,  TY_I16, TY_I32, TY_I64, TY_ISIZE,
    TY_F32, TY_F64,
    TY_COMPTIME_INT,
    TY_COMPTIME_FLOAT,
} TypeKind;

struct Type {
    TypeKind kind;
};

// Init: allocate the singleton Type for each primitive kind into the
// arena and stamp Sema's primitive type cache fields. Idempotent;
// safe to call once during sema_init after the arena is up.
void sema_types_init(struct Sema *s);

// Look up the Type* for a primitive name (e.g. "u8", "f32",
// "comptime_int"). Returns NULL for unknown names. The lookup is
// O(1) via Sema.primitive_types.
struct Type *type_for_primitive_name(struct Sema *s, uint32_t name_id);

// Human-readable spelling for diagnostics and dumps. Returns a
// pointer to a static string — do not free.
const char *type_name(const struct Type *t);

// True if `t` is one of TY_U8..TY_ISIZE / TY_COMPTIME_INT.
bool type_is_int(const struct Type *t);
// True if `t` is TY_F32, TY_F64, or TY_COMPTIME_FLOAT.
bool type_is_float(const struct Type *t);
// True if `t` is one of the unsigned int kinds.
bool type_is_unsigned(const struct Type *t);

#endif
