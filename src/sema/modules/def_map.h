#ifndef ORE_SEMA_DEF_MAP_H
#define ORE_SEMA_DEF_MAP_H

#include <stdbool.h>

#include "../ids/ids.h"

// DefMap construction.
//
// "DefMap" in this codebase is the populated state of a module's
// internal_scope and export_scope. Unlike rust-analyzer where DefMap
// is its own struct, ours doesn't need a separate aggregate — the
// scope tables on Sema already hold everything. The "build the def
// map" step is what def_map_collect_top_level does: walk the
// module's top-level AST, allocate a DefId per binding, register it
// into the module's scopes.
//
// Decoupled from modules.c so the walker can grow (member
// pre-seeding for type-shaped binds, top-level effect declarations,
// destructure-binds) without modules.c becoming a kitchen sink.

struct Sema;

// Walk `mid`'s top-level AST and populate its internal_scope and
// (for public binds) export_scope. Assumes the scopes already exist
// — caller is responsible for creating them (see modules.c).
//
// Returns true on success. False indicates a structural failure
// (duplicate top-level name, malformed AST). Diagnostics are emitted
// during the walk.
bool def_map_collect_top_level(struct Sema *s, ModuleId mid);

#endif // ORE_SEMA_DEF_MAP_H
