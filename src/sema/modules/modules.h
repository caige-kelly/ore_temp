#ifndef ORE_SEMA_MODULES_H
#define ORE_SEMA_MODULES_H

#include <stdbool.h>
#include <stdint.h>

#include "../../common/vec.h"
#include "../ids/ids.h"
#include "../query/query.h"
#include "../scope/scope.h"
#include "inputs.h"

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
// `input` is the InputId driving this module's source. INVALID for
// the prelude (no source file). For real modules, the input holds
// the source text + revision; query_module_ast lazily parses it.
//
// `ast` is populated by query_module_ast on demand. NULL until the
// first call (or after invalidation). Caching here is a courtesy —
// the canonical cache state is `input->ast_query`'s slot.
//
// `ast_fp` is the fingerprint of the last successful parse, used by
// the invalidation walker (Layer 7.5). Module-level slots compare
// their `verified_rev` against the input's `last_changed_rev` and
// the AST fingerprint to decide whether to re-parse.
//
// `internal_scope` holds every top-level decl plus imports — what
// code inside this module sees during resolution. `export_scope` is
// a strict subset: only `Visibility_public` decls. Path resolution
// from outside the module reads only the export scope.
struct ModuleInfo {
    InputId input;                // INVALID for prelude
    Vec *ast;                     // populated by query_module_ast; NULL until then
    Fingerprint ast_fp;           // last-known parse fingerprint
    ScopeId internal_scope;       // SCOPE_MODULE; parent = prelude.export_scope
    ScopeId export_scope;         // SCOPE_MODULE; parent = SCOPE_ID_INVALID
    Vec *imports;                 // Vec<struct ImportEntry>; NULL until populated

    // Layer 7.3 — per-decl lazy DefMap caches.
    //
    // top_level_index: Vec<struct TopLevelEntry>. Populated lazily
    // by query_top_level_index — a cheap AST scan with no DefId
    // allocation. NULL until first call.
    //
    // def_map_entries: HashMap name_id -> DefMapEntry*. Each entry
    // holds the on-demand DefId + its own slot (so per-name cycle
    // detection works). Populated incrementally by
    // query_def_for_name.
    Vec *top_level_index;
    HashMap def_map_entries;

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
// Allocate a fresh ModuleInfo for `input`. The AST is NOT parsed at
// creation time — parse-on-demand via query_module_ast.
// `is_prelude=true` builds the synthetic prelude module (input
// must be INPUT_ID_INVALID; no source file).
//
// Caches by the input's path_id so re-creating a module for the
// same input returns the existing ModuleId.
//
// Callers should not normally call this directly; use
// query_module_for_path which handles registration end-to-end.
ModuleId module_create(struct Sema *s, InputId input, bool is_prelude);

// === Queries ===
//
// query_module_ast: lazily parses the module's source on first call;
// re-parses when the input has been mutated since last successful
// parse. Returns NULL for the prelude (no source) and on parse
// failure.
//
// The slot lives on the InputInfo (`ast_query`), not the ModuleInfo,
// because the AST is a function of the source text — two modules
// pointing at the same input would share parse work, though the 1:1
// model means this rarely happens in practice.
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
