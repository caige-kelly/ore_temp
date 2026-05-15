#ifndef ORE_SEMA_TYPE_LAYOUT_H
#define ORE_SEMA_TYPE_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

#include "db/query/query.h"

// query_layout_of_type — compute the byte size and alignment of a
// type. Pure function of the type's structural identity; results are
// slot-cached per Type* (interned, so pointer-identity = type-identity)
// in `Sema.layout_of_type`.
//
// Mirrors Zig's `Type.abiSize` / `Type.abiAlignment` shape. The result
// is target-dependent for `usize` / `isize` and pointer-likes — we
// hardcode 64-bit ABI for now (every target we care about today is
// LP64). When we grow a real target abstraction, this is the layer
// that consults it.
//
// `is_known` distinguishes "computed and zero" from "unknown / cycle /
// error type". Aggregates that contain themselves by value cycle out
// here with on_cycle producing { .is_known = false } and an emitted
// "contains itself by value" diagnostic.
struct Layout {
    uint64_t size;        // bytes
    uint64_t align;       // alignment in bytes (power of two)
    bool is_known;        // false on cycle / error / unsupported kind
};

struct Sema;
struct Type;

// Per-Type layout cache entry. Keyed by Type* in Sema.layout_of_type.
// Owns the query slot so cycle / invalidation flow through the
// standard machinery.
struct LayoutEntry {
    struct Layout layout;
    struct QuerySlot query;
};

// Compute (or fetch from cache) the layout of `t`. Always returns a
// valid Layout struct — check `.is_known` before using `.size` /
// `.align`. Diagnoses by-value cycles via the `on_cycle` sentinel.
struct Layout query_layout_of_type(struct Sema* s, struct Type* t);

#endif
