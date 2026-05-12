#ifndef ORE_SEMA_MODULES_H
#define ORE_SEMA_MODULES_H

#include <stdbool.h>
#include <stdint.h>

#include "../../common/vec.h"
#include "../ids/ids.h"
#include "../query/query.h"
#include "../scope/scope.h"
#include "ast_id_map.h"
#include "inputs.h"
#include "../../common/stringpool.h"

// Modules — first-class compilation units.
//
// One ModuleInfo per .ore file. Holds the parsed top-level AST plus
// the module-internal and exported scopes. All scope/def construction
// for top-level items happens through query_module_def_map, which is
// the single point of entry for "what items exist in this module."
//
// The primitives module is a synthetic module (always at ModuleId{1})
// that holds compiler-built-in primitive types (`u8`, `i32`, ...).
// Every user module's internal_scope parents to the primitives
// module's export_scope so primitive names resolve without any
// user-side import. See primitives.c for the immutability contract.
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
    StrId path_id;
    StrId alias_name_id;
    struct Span span;
    ModuleId resolved;       // populated by query_module_for_path
};

// === ModuleInfo ===
//
// Slot 1 of Sema.modules_table is the primitives module. User modules
// occupy slots 2..N. ModuleId{0} is INVALID.
//
// `input` is the InputId driving this module's source. INVALID for
// the primitives module (no source file). For real modules, the
// input holds the source text + revision; query_module_ast lazily
// parses it.
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
    InputId input;                // INVALID for the primitives module
    Vec *ast;                     // populated by query_module_ast; NULL until then
    Fingerprint ast_fp;           // last-known parse fingerprint
    ScopeId internal_scope;       // SCOPE_MODULE; parent = primitives.export_scope
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

    // Stable identities for top-level items. Rebuilt inside
    // query_top_level_index alongside `top_level_index`. AstIds are
    // hash((kind, name))-derived and stable across reparses — same
    // (kind, name) → same AstId regardless of byte position in the
    // file. Top-level DefInfo records carry their AstId so
    // `def_origin` can find the current revision's Bind node in
    // O(1), even when the item has shifted position. See
    // `ast_id_map.h` for the rust-analyzer parallel.
    struct AstIdMap ast_id_map;

    bool is_primitives;
    bool resolving;               // true while def_map is on the import stack
                                  // (mirrored by the query slot's RUNNING state;
                                  // kept here for fast import-cycle checks)
    bool resolved;                // true after def_map completed successfully
    struct QuerySlot def_map_query;
    struct QuerySlot exports_query;

    // top_level_index's own slot. Records a dep on QUERY_MODULE_AST
    // so a re-parse cascades through the invalidation walker and
    // clears the cached `top_level_index` Vec (which holds borrowed
    // Expr* pointers into the now-stale AST).
    struct QuerySlot top_level_query;

    // node_to_decl_index's slot. The body populates s->node_to_decl
    // with (NodeId → enclosing top-level DefId) entries for every
    // node in this module's AST. Consumers (query_node_to_decl,
    // query_scope_for_node) call this query to record a dep, then
    // read the populated map. Without this slot, the lookup
    // functions would be driver-tracked-only (B21 — fixed).
    struct QuerySlot node_to_decl_index_query;
};

// === Module construction ===
//
// Allocate a fresh ModuleInfo for `input`. The AST is NOT parsed at
// creation time — parse-on-demand via query_module_ast.
// `is_primitives=true` builds the synthetic primitives module (input
// must be INPUT_ID_INVALID; no source file).
//
// Caches by the input's path_id so re-creating a module for the
// same input returns the existing ModuleId.
//
// Callers should not normally call this directly; use
// query_module_for_path which handles registration end-to-end.
ModuleId module_create(struct Sema *s, InputId input, bool is_primitives);

// === Queries ===
//
// query_module_ast: lazily parses the module's source on first call;
// re-parses when the input has been mutated since last successful
// parse. Returns NULL for the primitives module (no source) and on
// parse failure.
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
ModuleId query_module_for_path(struct Sema *s, StrId path_id,
                               struct Span span);

// Reverse-lookup: given a Span (whose file_id corresponds to an
// InputId), find the ModuleId that owns that input. Returns
// MODULE_ID_INVALID if no match. Used for cross-module checks like
// field visibility, where the access site's module is derived from
// the expression's source location.
ModuleId module_for_span(struct Sema *s, struct Span span);

// Initialize the primitives module — registers `u8`, `i32`, `bool`,
// etc. into a synthetic module that every user module's
// internal_scope parents to. Call once during sema_init before any
// user modules are registered. Idempotent — re-calls are no-ops.
// The primitives module is sealed after this returns; see the
// immutability contract at the top of primitives.c. Lives in
// primitives.c.
void primitives_init(struct Sema *s);

#endif // ORE_SEMA_MODULES_H
