#ifndef SEMA_H
#define SEMA_H

#include <stdbool.h>
#include <stdint.h>

#include "./storage/arena.h"
#include "./storage/hashmap.h"
#include "./storage/stringpool.h"
#include "./storage/vec.h"
#include "../parser/ast.h"
#include "ids/ids.h"
#include "query/query.h"
#include "query/query_engine.h"
#include "intern_pool/intern_pool.h" 
#include "request/cancel.h"

struct db {
    // storage
    Arena global_arena;  // Never reset
    Arena scratch_arena; // resets
    InternPool strings;  // Global string dedup
    TypePool types;      // Global type dedup
    ValuePool values;    // Global value dedup

    
    Vec sources; // SourceId -> { path, source_text, line_index }

    // Map of ModuleId -> ASTStore*
    Vec module_asts;

    // Everything here is indexed by DefId.idx
    struct {
        Vec names;           // StrId
        Vec parent_modules;  // ModuleId
        Vec kinds;           // enum { FN, STRUCT, VAR, etc }
        Vec signatures;      // TypeId (for fns/vars)
        Vec values;          // ValueId
        Vec effects;         // EffectId
        Vec handler;         // HandlerId
        Vec file;            // FileId
        Vec node;            // NodeId
        Vec extras;          // NodeExtras
    } defs;

    // 5. THE QUERY ENGINE (The "Controller")
    QueryEngine query_engine; // Holds the cache slots and dependency graph

    // 6. THE SEED CONSTANTS (Global PKs)
    struct {
        TypeId u8_t;
        TypeId u16_t;
        TypeId u32_t;
        TypeId u64_t;
        TypeId usize_t;
        TypeId const_u8_t;
        TypeId const_u16_t;
        TypeId const_u32_t;
        TypeId const_u64_t;
        TypeId const_usize_t;

        TypeId i8_t;
        TypeId i16_t;
        TypeId i32_t;
        TypeId i64_t;
        TypeId isize_t;
        TypeId const_i8_t;
        TypeId const_i16_t;
        TypeId const_i32_t;
        TypeId const_i64_t;
        TypeId const_isize_t;

        TypeId f32_t;
        TypeId f64_t;
        TypeId const_f32_t;
        TypeId const_f64_t;

        // Other
        TypeId cint_t;
        TypeId cfloat_t;
        TypeId unknown_t;
        TypeId error_t;
        TypeId void_t;
        TypeId noreturn_t;
        TypeId return_t; 
        TypeId slice_t;
        TypeId array_t;
        TypeId bool_t;
        TypeId type_t;
        TypeId module_t;
    } primitives;
};
// Lifecycle. Sema owns its arenas, string pool, diagnostics bag,
// and source map. `sema_init` brings up the entire database; the
// LSP shell and the CLI driver both call it.
void db_init(struct db* s);
void db_free(struct db* s);

#endif // SEMA_H
