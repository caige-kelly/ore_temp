#ifndef ORE_SEMA_TYPE_DECL_INFO_H
#define ORE_SEMA_TYPE_DECL_INFO_H

#include "db/ids/ids.h"
#include "db/query/query.h"

struct Sema;
struct Type;

// Per-DefId sema cache. Owns the query slots for everything sema
// computes about a single declaration: its type, its effect
// signature, its body's effect set, etc. Stage E.1 only uses the
// `type_query` slot; the rest are reserved for downstream stages.
//
// Lifetime: arena-allocated, lookup-or-create via `sema_decl_info`.
struct SemaDeclInfo {
    struct QuerySlot type_query;
    struct Type *type;     // memoized result of query_type_of_decl
};

// Lookup-or-create the SemaDeclInfo for `def`. Returns NULL on
// invalid def or allocation failure. Stable pointer for the lifetime
// of the Sema database.
struct SemaDeclInfo *sema_decl_info(struct Sema *s, DefId def);

#endif
