#ifndef ORE_SEMA_MODULES_H
#define ORE_SEMA_MODULES_H

#include <stdbool.h>
#include <stdint.h>

#include "../../common/vec.h"
#include "../ids/ids.h"
#include "../query/query.h"
#include "../scope/scope.h"

// Modules — first-class compilation units.
//
// One ModuleInfo per .ore file. Holds the parsed top-level AST plus
// the module-internal and exported scopes. All scope/def construction
// for top-level items happens through query_module_def_map, which is
// the single point of entry for "what items exist in this module."
//
// The prelude is a synthetic module (always at ModuleId{1}) that
// holds builtin primitive types. Every module's internal_scope
// parents to the prelude's export_scope so primitive names resolve
// without per-module boilerplate.
//
// Module loading is demand-driven: query_module_for_path triggers a
// parse on first access. Cycles are detected by the query stack —
// `A imports B imports A` shows up as a QUERY_BEGIN_CYCLE in
// query_module_def_map and is reported as E0110.

struct Sema;

// === ImportEntry ===
//
// Recorded on a ModuleInfo for every successful @import. The path is
// the canonical filesystem path (interned in StringPool). The alias
// is what the importing source named the module under (also a
// pool_id). Span is the @import call site for diagnostics.
struct ImportEntry {
    uint32_t path_id;
    uint32_t alias_name_id;
    struct Span span;
    ModuleId resolved;       // populated by query_module_for_path
};

// === ModuleInfo ===
//
// Slot 1 of Sema.modules_table is the prelude. User modules occupy
// slots 2..N. ModuleId{0} is INVALID.
//
// `internal_scope` holds every top-level decl plus imports — what
// code inside this module sees during resolution. `export_scope` is
// a strict subset: only `Visibility_public` decls. Path resolution
// from outside the module reads only the export scope.
struct ModuleInfo {
    uint32_t path_id;             // canonical path interned in pool
    Vec *ast;                     // top-level Vec<struct Expr*>; NULL for prelude
    ScopeId internal_scope;       // SCOPE_MODULE; parent = prelude.export_scope
    ScopeId export_scope;         // SCOPE_MODULE; parent = SCOPE_ID_INVALID
    Vec *imports;                 // Vec<struct ImportEntry>; NULL until populated
    bool is_prelude;
    bool resolving;               // true while def_map is on the import stack
                                  // (mirrored by the query slot's RUNNING state;
                                  // kept here for fast import-cycle checks)
    bool resolved;                // true after def_map completed successfully
    struct QuerySlot def_map_query;
    struct QuerySlot exports_query;
};

// === Module construction ===
//
// Allocate a fresh ModuleInfo, register scopes, return its
// ModuleId. AST may be NULL — query_module_for_path/parsing fills it
// in for lazy loads. The internal_scope's parent is set to the
// prelude's export scope unless `is_prelude` is true.
//
// Callers should not normally call this directly; use
// query_module_for_path which handles caching by path.
ModuleId module_create(struct Sema *s, uint32_t path_id, Vec *ast,
                       bool is_prelude);

// === Queries ===
//
// query_module_ast: returns the top-level Vec<Expr*>. Today this is
// just a getter (parsing happens at module_create); the query shape
// is preserved so future lazy-parse plumbing slots in cleanly.
Vec *query_module_ast(struct Sema *s, ModuleId mid);

// query_module_def_map: walks the top-level AST and registers every
// item into internal_scope (and export_scope if public). Pre-seeds
// member scopes for type-shaped binds so forward refs work. Idempotent
// via the slot. Cycles report E0110. Returns true on success.
bool query_module_def_map(struct Sema *s, ModuleId mid);

// query_module_exports: ensures def_map has run, returns export_scope.
// Returns SCOPE_ID_INVALID on def_map failure.
ScopeId query_module_exports(struct Sema *s, ModuleId mid);

// query_module_for_path: resolves a path string (typically interned
// from a string literal in @import) to a ModuleId. Loads + parses on
// first call; caches by canonical path on Sema.module_by_path.
//
// `span` is the @import call site, used for diagnostics on parse
// failures and cycles. Returns MODULE_ID_INVALID on failure.
ModuleId query_module_for_path(struct Sema *s, uint32_t path_id,
                               struct Span span);

// Initialize the prelude module. Call once during sema_new before
// any user modules are registered. Idempotent — re-calls are no-ops.
// Lives in prelude.c.
void prelude_init(struct Sema *s);

#endif // ORE_SEMA_MODULES_H
