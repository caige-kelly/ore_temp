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

struct Source {
    SourceId id;      // Identity
    FileId file_id;   // Path on disk
    StrId text;       // Content of file
    uint64_t hash;    // Content hash
    uint32_t version; // Incremental version from the LSP
};

struct db {
    // storage
    Arena global_arena;   // Never resets
    Arena scratch_arena;  // resets

    StringPool strings;   // Global string dedup + owned Arena
    InternPool intern;    // Unified storage for types and values.

    
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
#define X(lower, UPPER, ...) IpIndex lower##_t;
#include "intern_pool/ip_primitives.def"
#undef X
    } primitives;
};
// Lifecycle. Sema owns its arenas, string pool, diagnostics bag,
// and source map. `sema_init` brings up the entire database; the
// LSP shell and the CLI driver both call it.
void db_init(struct db* s);
void db_free(struct db* s);

#endif // SEMA_H
