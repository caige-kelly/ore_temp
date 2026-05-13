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

typedef struct {
        Arena* arena;
        Vec* diags;         // Vec<Diag>
        size_t error_count;
        size_t warning_count;
    } DiagBag;

struct db {
    // 1. FUNDAMENTAL STORAGE
    Arena global_arena;      // Never reset
    StringPool strings;      // Global dedup
    InternPool types;        // Global type dedup (R4)
    
    // 2. THE INPUTS (Your new SourceMap)
    Vec inputs;              // InputId -> { path, source_text, line_index }

    // 3. THE AST DATABASE (Layer 1)
    // Map of ModuleId -> ASTStore*
    // This is where the Parser's hard work lives.
    Vec module_asts; 

    // 4. THE SEMANTIC SIDE-TABLES (The "Real" Database)
    // Everything here is indexed by DefId.idx
    struct {
        Vec names;           // StrId
        Vec parent_modules;  // ModuleId
        Vec kinds;           // enum { FN, STRUCT, VAR, etc }
        Vec signatures;      // TypeId (for fns/vars)
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
