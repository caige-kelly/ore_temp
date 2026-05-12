#ifndef SEMA_H
#define SEMA_H

#include <stdbool.h>
#include <stdint.h>

#include "../support/common/arena.h"
#include "../support/common/hashmap.h"
#include "../support/common/stringpool.h"
#include "../support/common/vec.h"
#include "../parser/ast.h"
#include "../support/diag/diag.h"
#include "ids/ids.h"
#include "query/query.h"
#include "intern_pool/intern_pool.h" 
#include "request/cancel.h"

struct db {
    // --- Infrastructure ---
    Arena arena;
    Arena pass_arena;
    StringPool pool;
    struct DiagBag diags;
    struct SourceMap source_map;

    // --- Core ID Tables (Layer 1) ---
    // These tables issue the sequential dense IDs.
    Vec defs_table;      // DefId.idx -> struct DefInfo
    Vec scopes_table;    // ScopeId.idx -> struct ScopeInfo
    Vec modules_table;   // ModuleId.idx -> struct ModuleInfo
    Vec inputs_table;    // InputId.idx -> struct InputInfo

    // --- Query Framework (Layer 2) ---
    uint64_t current_revision;
    bool invalidation_enabled;
    Vec query_stack;
    struct CancelToken *active_cancel;
    uint64_t request_revision;

    // --- String & Path Indexes (Sparse: HashMaps) ---
    HashMap module_by_path;
    HashMap inputs_by_path;
    HashMap primitive_types; // StrId -> TypeId

    // --- The Intern Pool (Layer R4) ---
    InternPool intern_pool;
    Vec types_by_ip;         // IpIndex -> struct Type* (soon just raw layout bytes)

    // --- Dense Side-Tables (Replaced HashMaps!) ---
    // All of these are O(1) lookups: vec.data[id]
    
    // Keyed by DefId.idx
    Vec decl_info;
    Vec fn_signatures;
    Vec param_locators;
    Vec struct_signatures;
    Vec field_locators;
    Vec enum_signatures;
    Vec variant_locators;
    Vec decl_hir;
    Vec effect_ops_cache;
    Vec fn_scope_index_cache;
    Vec body_stores;
    Vec call_cache;

    // Keyed by ModuleId.idx
    Vec module_hir;
    Vec span_index_by_module;

    // Keyed by TypeId (IpIndex.v)
    Vec layout_of_type;

    // --- Sparse / Composite Relational Tables (Keep HashMaps) ---
    HashMap struct_field_defs;     // (DefId << 32) | index -> DefId
    HashMap enum_variant_defs;     // (DefId << 32) | index -> DefId
    HashMap resolve_ref_entries;   // (NodeId << 4) | NS -> ResolveRefEntry
    HashMap resolve_path_entries;  // (NodeId << 4) | NS -> ResolvePathEntry
    HashMap instantiation_buckets; // DefId -> Vec<Instantiation*>

    // --- Pre-interned IDs ---
    StrId name_sizeOf;
    StrId name_alignOf;
    StrId name_intCast;
    StrId name_TypeOf;
    StrId name_typeName;
    ModuleId primitives_module;

    // --- Global Types (Replaced Pointers with TypeId) ---
    TypeId unknown_type;
    TypeId error_type;
    TypeId void_type;
    TypeId noreturn_type;
    TypeId bool_type;
    TypeId u8_type;
    TypeId const_u8_type;
    TypeId i32_type;
    TypeId type_type;
    TypeId module_type;
    // ... etc
    
    // --- Interpreter Context ---
    struct ComptimeEnv* current_env;
    struct EvidenceVector* current_evidence;
    int comptime_call_depth;
    int64_t comptime_body_evals;
};

// Lifecycle. Sema owns its arenas, string pool, diagnostics bag,
// and source map. `sema_init` brings up the entire database; the
// LSP shell and the CLI driver both call it.
void db_init(struct Sema* s);
void db_free(struct Sema* s);

#endif // SEMA_H
