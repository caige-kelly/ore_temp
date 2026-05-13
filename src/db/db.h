#ifndef SEMA_H
#define SEMA_H

#include <stdbool.h>
#include <stdint.h>

#include "../support/common/arena.h"
#include "../support/common/hashmap.h"
#include "../support/common/stringpool.h"
#include "../support/common/vec.h"
#include "../parser/ast.h"
#include "ids/ids.h"
#include "query/query.h"
#include "intern_pool/intern_pool.h" 
#include "request/cancel.h"

typedef struct {
        Arena* arena;
        Vec* diags;         // Vec<Diag>
        size_t error_count;
        size_t warning_count;
    } DiagBag;

struct db {
    // --- Infrastructure ---
    Arena arena;
    Arena pass_arena;
    StringPool pool;

    DiagBag diag_bag;

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

    // --- Global Types  ---
    struct {
        // u's
        TypeId u8_type;
        TypeId u16_type;
        TypeId u32_type;
        TypeId u64_type;
        TypeId u128_type;
        TypeId usize_type;
        TypeId const_u8_type;
        TypeId const_u16_type;
        TypeId const_u32_type;
        TypeId const_u64_type;
        TypeId const_u128_type;
        TypeId const_usize_type;

        // i's
        TypeId i8_type;
        TypeId i16_type;
        TypeId i32_type;
        TypeId i64_type;
        TypeId i128_type;
        TypeId isize_type;
        TypeId const_i8_type;
        TypeId const_i16_type;
        TypeId const_i32_type;
        TypeId const_i64_type;
        TypeId const_i128_type;
        TypeId const_isize_type;

        // f's
        TypeId f32_type;
        TypeId f64_type;
        TypeId const_f64_type;
        TypeId const_f32_type;

        // Other
        TypeId cint_type;
        TypeId cfloat_type;
        TypeId unknown_type;
        TypeId error_type;
        TypeId void_type;
        TypeId noreturn_type;
        TypeId return_type; 
        TypeId slice_type;
        TypeId array_type;
        TypeId bool_type;
        TypeId type_type;
        TypeId module_type;
    } primitives;
    
    // --- Interpreter Context ---
    struct ComptimeEnv* current_env;
    struct EvidenceVector* current_evidence;
    int comptime_call_depth;
    int64_t comptime_body_evals;
};

// Lifecycle. Sema owns its arenas, string pool, diagnostics bag,
// and source map. `sema_init` brings up the entire database; the
// LSP shell and the CLI driver both call it.
void db_init(struct db* s);
void db_free(struct db* s);

#endif // SEMA_H
