#ifndef TYPEPOOL_H
#define TYPEPOOL_H

#include "../ids/ids.h"

typedef enum {
    TYPE_NONE,
    TYPE_PRIMITIVE,
    TYPE_POINTER,
    TYPE_SLICE,
    TYPE_ARRAY,
    TYPE_FUNCTION,
    TYPE_STRUCT_REF, // Points to a DefId
    TYPE_EFFECTFUL,  // A type wrapped with effect constraints
} TypeKind;

typedef struct {
    TypeKind kind;
    union {
        uint8_t primitive; // i32, u64, etc.
        TypeId  ptr_to;
        struct { TypeId element; uint32_t len; } array;
        struct { TypeId *params; uint32_t count; TypeId ret; } func;
        DefId   struct_def;
    } as;
} TypeDescriptor;

#endif //TYPEBOOL_H