#ifndef ORE_SEMA_TYPE_H
#define ORE_SEMA_TYPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../ids/ids.h"

struct Sema;

// Type representation — Stage E.2.
//
// Adds compound types — function, pointer, slice, fixed-size array —
// on top of E.1's primitives. All Types are interned: there is
// exactly one canonical Type* for any given (kind, payload). Type
// equality is pointer equality. The interners live in `intern.c`
// and back the `type_fn` / `type_ptr` / `type_slice` / `type_array`
// constructors below.
//
// `comptime_int` and `comptime_float` remain transient — they exist
// as real types so coercion checks can flow naturally, but they
// should always elaborate to a concrete type at use sites (bind
// annotation, function arg, return statement).

typedef enum {
    TY_ERROR,            // sentinel for "type unknown / error recovery"
    TY_VOID,
    TY_NORETURN,         // expression that diverges (return / break / continue)
    TY_BOOL,
    TY_U8,  TY_U16, TY_U32, TY_U64, TY_USIZE,
    TY_I8,  TY_I16, TY_I32, TY_I64, TY_ISIZE,
    TY_F32, TY_F64,
    TY_COMPTIME_INT,
    TY_COMPTIME_FLOAT,
    TY_NIL,              // singleton type of the `nil` literal. Coerces
                         //  to any optional (`?T`) or any pointer
                         //  (`^T` / `[^]T` / `[]T`). Has no payload —
                         //  there's only one nil. Mirrors Zig's
                         //  `@TypeOf(null)` (the special "null literal"
                         //  type that exists for coerce purposes only).
    TY_TYPE,             // the kind-of-types — `type` primitive. The result
                         // type of `@TypeOf(x)` and the type of `Foo` itself
                         // when `Foo :: struct {...}` is read as a value.
    // ---- Compound kinds ----
    TY_FN,               // fn(params...) -> ret
    TY_PTR,              // ^T  /  ^const T   (single pointer to one item)
    TY_MANY_PTR,         // [^]T  /  [^]const T (many-pointer; pointer arith
                         // allowed, no length carried). Mirrors Zig's [*]T —
                         // the runtime ABI of `slice.ptr` and what `[^]T`
                         // resolves to in type position.
    TY_SLICE,            // []T  /  []const T
    TY_ARRAY,            // [N]T (N is comptime-known)
    TY_OPTIONAL,         // ?T — value-or-nil. Coerce: T → ?T (lift) and
                         //  nil → ?T. Unwrapping is op-shaped (`?` /
                         //  `orelse`); type-side semantics live here.
    // ---- Nominal user-defined kinds (Stage E.3) ----
    //
    // Identity-only: the Type carries just the DefId of the declaration.
    // Two TY_STRUCTs are pointer-equal iff their DefIds match. Field /
    // variant detail lives in side tables (StructSignature /
    // EnumSignature in sema/type/decl_data.h), populated by their own
    // queries. Mirrors rust-analyzer's `TyKind::Adt(AdtId, _)` and
    // breaks the cycle for recursive shapes (`Node :: struct { next:
    // ^Node }`) — building the TY_STRUCT doesn't recurse into fields.
    TY_STRUCT,           // user-defined struct
    TY_ENUM,             // user-defined enum
} TypeKind;

struct Type {
    TypeKind kind;
    union {
        struct {
            struct Type **params;     // arena-owned array of param types
            size_t param_count;
            struct Type *ret;
        } fn;
        struct {
            struct Type *elem;
            bool is_const;            // ^const T vs ^T
        } ptr;
        struct {
            struct Type *elem;
            bool is_const;            // [^]const T vs [^]T
        } many_ptr;
        struct {
            struct Type *elem;
            bool is_const;            // []const T vs []T
        } slice;
        struct {
            struct Type *elem;
            uint64_t size;            // const-evaluated [N]T length
        } array;
        struct {
            struct Type *elem;        // ?T — the inner value type
        } optional;
        struct {
            DefId def;                // identity for TY_STRUCT
        } struct_;
        struct {
            DefId def;                // identity for TY_ENUM
        } enum_;
    };
};

// === Init ===
//
// Allocates the singleton Type for each primitive kind and stamps
// Sema's primitive cache fields. Also initializes the compound-type
// interners (called from sema_intern_init at sema_init time).
void sema_types_init(struct Sema *s);

// === Primitive lookup ===
struct Type *type_for_primitive_name(struct Sema *s, uint32_t name_id);

// === Compound type constructors (interning) ===
//
// Each returns the canonical Type* for the requested shape. Two calls
// with structurally-equal arguments return the same pointer. Lives in
// `intern.c`; the interner state is on Sema (Sema.fn_types, etc.).
//
// `params` is borrowed by `type_fn` if a fresh Type is constructed —
// the params array is copied into the arena. Callers may safely free
// or stack-allocate their original.

struct Type *type_fn(struct Sema *s, struct Type **params, size_t param_count,
                     struct Type *ret);
struct Type *type_ptr(struct Sema *s, struct Type *elem, bool is_const);
struct Type *type_many_ptr(struct Sema *s, struct Type *elem, bool is_const);
struct Type *type_slice(struct Sema *s, struct Type *elem, bool is_const);
struct Type *type_array(struct Sema *s, struct Type *elem, uint64_t size);
struct Type *type_optional(struct Sema *s, struct Type *elem);

// Identity-only nominal types. Interned by DefId — two calls with the
// same `def` return the same Type*.
struct Type *type_struct(struct Sema *s, DefId def);
struct Type *type_enum(struct Sema *s, DefId def);

// === Predicates ===
bool type_is_int(const struct Type *t);
bool type_is_float(const struct Type *t);
bool type_is_unsigned(const struct Type *t);
bool type_is_numeric(const struct Type *t);   // int OR float
bool type_is_comptime(const struct Type *t);  // comptime_int / comptime_float

// === Human-readable name ===
//
// Short kind name only — for compound types you want `type_to_string`
// from display.h, which renders the full structure (e.g.
// `fn(i32, i32) -> i32`).
const char *type_name(const struct Type *t);

#endif
