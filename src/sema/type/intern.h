#ifndef ORE_SEMA_TYPE_INTERN_H
#define ORE_SEMA_TYPE_INTERN_H

// Compound-type interning.
//
// Post-R4 cleanup, every `type_*` constructor in this module
// (declared in type.h) routes through the unified intern pool —
// `s->intern_pool`. Identity is `IpIndex`; the returned
// `struct Type *` is a bridge object kept on `s->types_by_ip`.
// Two structurally-equivalent constructor calls return the same
// `Type *` because the pool deduplicates by content.
//
// No per-kind hashmap init is needed — the pool itself is
// initialized once in `sema_init` via `ip_init`. This header is
// retained as a documentation anchor; the implementation lives in
// `intern.c`.

#endif
