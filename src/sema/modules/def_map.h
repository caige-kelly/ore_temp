#ifndef ORE_SEMA_DEF_MAP_H
#define ORE_SEMA_DEF_MAP_H

#include <stdbool.h>
#include <stdint.h>

#include "../../common/vec.h"
#include "../../parser/ast.h"
#include "../ids/ids.h"
#include "../query/query.h"
#include "../scope/scope.h"

// DefMap construction — split into a cheap top-level index plus
// per-name lazy DefId allocation.
//
// Editing one fn's body should NOT invalidate the def_map for any
// other decl in the module. The index records what names exist
// without committing to DefIds; per-name slots commit lazily so
// only the affected entries rebuild on a relevant edit.
//
// `query_module_def_map` is retained as the batch convenience —
// useful for `--dump-defs` and tests. It walks the top-level index
// and calls `query_def_for_name` for each entry.

struct Sema;

// One row of the cheap top-level index. `node` is the source AST
// expression that introduces the name (typically expr_Bind). The
// `is_destructure` flag distinguishes naked binds (one name) from
// destructure binds (multiple bindings under one pattern); the lazy
// allocator handles each shape.
struct TopLevelEntry {
    uint32_t name_id;
    struct Expr *node;
    Visibility vis;
    struct Span span;
    bool is_destructure;
};

// Per-name lazy DefId construction state. Stored in a per-module
// HashMap keyed by name_id. The query slot lives here so each name
// gets its own cycle-detection scope.
struct DefMapEntry {
    uint32_t name_id;
    DefId def;                 // INVALID until query_def_for_name runs
    struct QuerySlot query;
};

// Walk the module's AST top-level once and produce the index. Cheap:
// no DefId allocation, no scope mutation. Cached on ModuleInfo;
// re-runs only on AST re-parse (the invalidation walker takes care
// of that once 7.5 lands).
Vec *query_top_level_index(struct Sema *s, ModuleId mid);

// Resolve `name_id` to its DefId in the given module, allocating
// the DefId on first call. Looks the name up in the top-level
// index, populates DefInfo, inserts into the module's
// internal_scope (and export_scope if public). Subsequent calls
// return the cached DefId.
//
// Returns DEF_ID_INVALID if `name_id` isn't a top-level name in
// `mid`.
DefId query_def_for_name(struct Sema *s, ModuleId mid, uint32_t name_id);

// Convenience: drive every top-level entry through query_def_for_name.
// Used by batch tools and the existing query_module_def_map shim.
// Returns true on success (every name resolved without conflict).
bool def_map_collect_top_level(struct Sema *s, ModuleId mid);

#endif // ORE_SEMA_DEF_MAP_H
