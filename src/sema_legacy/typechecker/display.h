#ifndef ORE_SEMA_TYPE_DISPLAY_H
#define ORE_SEMA_TYPE_DISPLAY_H

#include <stddef.h>

struct Sema;
struct Type;

// Render a Type to a human-readable string. For primitives, the
// output matches `type_name`. For compound types, structure is
// rendered: `fn(i32, i32) -> i32`, `^const u8`, `[10]u8`, `[]i32`.
//
// Writes into `buf` (caller-owned, must be at least 32 bytes;
// 128–256 is comfortable for everyday types). Returns `buf` so the
// call composes with printf-style formatters. On overflow the
// output is truncated; never reads past `buflen`.
//
// The pointer comparison guarantee from interning means equal
// types always render identically. Two different types may render
// to the same string only if they're truly structurally equal
// (which would be a bug — interning would have collapsed them).
const char *type_to_string(struct Sema *s, const struct Type *t, char *buf,
                           size_t buflen);

#endif
