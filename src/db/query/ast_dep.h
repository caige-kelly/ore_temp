#ifndef ORE_DB_QUERY_AST_DEP_H
#define ORE_DB_QUERY_AST_DEP_H

/*
    AST-dependency recording helpers.

    Any query body that reads expression structure (signature queries,
    const-eval, comptime predicate, etc.) must record a dep on the owning
    module's AST slot. Without that dep, edits to the source don't
    invalidate the cached result — the revalidation walker sees no
    changed inputs and serves stale data.

    Two entry points, picked by what handle the caller has on hand:

      db_record_ast_dep_for_def(s, def)
          Caller has a DefId. Walks DefId → defs.parent_modules → ModuleId,
          fetches the owning ModuleInfo, then invokes db_query_begin on
          QUERY_MODULE_AST with &mod->id as the (pointer-stable) key. The
          AST slot is treated as an LSP-managed input — assumed already
          DONE; the begin call returns CACHED and the dep gets recorded
          on the calling parent's frame.

      db_record_ast_dep_for_span(s, span)
          Caller has a CompactSpan (typically the span of an Expr or any
          other span-bearing node). Resolves the FileId to a Module via
          db_module_for_file, then records the dep the same way as
          _for_def above.

    Both are no-ops when called outside an active query frame — record_dep_on_parent
    is defensive in the same way. The macro DB_READ_UNTRACKED (declared
    in db/query/query.h) covers the "intentional non-query read" case;
    no separate macro lives here.
*/

#include "../ids/ids.h"             // DefId
#include "../workspace/module_info.h"  // CompactSpan

struct db;

// Record a dep on the AST of `def`'s owning module. Asserts on out-of-
// range DefId; silently no-ops on the NONE sentinel or if the def has
// no parent module recorded. Asserts on the slot not being available
// (the LSP must have stamped MODULE_AST as an input before any query
// touches it) — a NULL slot is a programming error, not a runtime case.
void db_record_ast_dep_for_def(struct db *s, DefId def);

// Record a dep on the AST of the module backing `span.file`. Silently
// no-ops if no module owns that file (e.g. virtual buffer not yet
// registered).
void db_record_ast_dep_for_span(struct db *s, CompactSpan span);

#endif // ORE_DB_QUERY_AST_DEP_H
