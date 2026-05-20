#ifndef ORE_SEMA_RESOLVE_DUMP_H
#define ORE_SEMA_RESOLVE_DUMP_H

#include "db/ids/ids.h"

struct Sema;

// Print the module's top-level def map and recursively resolve every
// Ident in each decl's subtree, printing `name/ns -> def=N`. Used by
// `--dump-resolve`. Drives query_def_for_name + query_resolve_ref so
// it doubles as a smoke test of the resolution layer.
void dump_resolve(struct Sema *s, ModuleId mid);

#endif
