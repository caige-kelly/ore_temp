#ifndef ORE_DB_H
#define ORE_DB_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "./storage/arena.h"
#include "./storage/hashmap.h"
#include "./storage/stringpool.h"
#include "./storage/vec.h"
#include "./ids/ids.h"
#include "./intern_pool/intern_pool.h"
#include "./query/query.h"

// Defined in workspace/module_info.h. db only sees pointers.
struct ModuleInfo;


/* ============================================================================
   Column value types.

   Enums and small structs whose only consumer is the db.* SoA columns. Defined
   here next to the columns that use them.
   ============================================================================ */

typedef enum : uint8_t {
    VIS_PRIVATE = 0,
    VIS_PUBLIC,
    VIS_INTERNAL,
} Visibility;

typedef enum : uint8_t {
    SCOPE_NONE = 0,
    SCOPE_MODULE,
    SCOPE_FUNCTION,
    SCOPE_BLOCK,
    SCOPE_STRUCT,
    SCOPE_ENUM,
} ScopeKind;

// One name → DefId entry in a scope's decl list. Scope decls are stored
// flat (see scopes.decl_pool / scopes.decl_offsets); a scope's decl slice
// is decl_pool[decl_offsets[s] .. decl_offsets[s+1]].
typedef struct {
    StrId name;
    DefId def;
} DeclEntry;


/* ============================================================================
   Source.

   One entry per loaded file or virtual buffer. db.sources is a SoA Vec
   indexed by SourceId.idx; the matching FileId resolves to the same row
   via file_id_local(fid).
   ============================================================================ */

struct Source {
    FileId    file_id;
    StrId     path;        // interned path; synthetic name for virtual buffers
    char     *text;        // owned by db.arena (or by a virtual-buffer generator)
    uint32_t  text_len;
    uint64_t  hash;        // FNV-1a over text
    uint32_t  version;     // LSP-tracked revision; bumped on every edit
};


/* ============================================================================
   The database.

   Single struct that owns every long-lived piece of compiler state. Layout
   philosophy:

   - Storage at the top (arena, intern, strings) so the hot init path stays
     in one cache region.
   - All id-keyed data is SoA — columns parallel-indexed by the id, no
     AoS structs hiding cache-cold fields behind cache-hot ones.
   - The two sparse-key caches (paths, comptime call args) are the only
     HashMaps on db. Every other "X → Y" lookup is a direct Vec index.
   - Per-decl query slots live in SoA columns next to defs so the
     invalidation walker iterates one slot kind in a tight loop.
   - Per-module data (ASTStore, Big Four side-tables, AstIdMap,
     top_level_index, per-module slots) lives on struct ModuleInfo;
     db holds Vec<ModuleInfo*> with ModuleInfos allocated in db.arena for
     locality and pointer stability.
   ============================================================================ */

struct db {
    /* ------------------------------------------------------------------------
       Storage.
       ------------------------------------------------------------------------ */

    // Long-lived arena. Owns: query slots, def metadata, intern-pool payloads,
    // ModuleInfo structs, source text buffers, scope decl pool.
    Arena         arena;

    // Per-request scratch. The LSP request handler resets this at request
    // boundary. Comptime evaluations get their own per-call arenas, not this
    // one; this is for transient strings/Vecs built during query bodies.
    Arena         request_arena;

    // Global string interner.
    StringPool    strings;

    // Unified intern pool: types + comptime values + effect rows. Every
    // type/value/effect-row identity in the system is an IpIndex.
    InternPool    intern;

    /* ------------------------------------------------------------------------
       Workspace.
       ------------------------------------------------------------------------ */

    // Indexed by SourceId.idx. Physical files and virtual buffers share this
    // table; resolution dispatches on file_id_is_virtual.
    Vec           sources;           // Vec<Source>

    // Indexed by ModuleId.idx. Each ModuleInfo is allocated in db.arena.
    // The Vec holds pointers — the underlying ModuleInfo objects never move,
    // so pointers stored elsewhere stay valid across module-table growth.
    Vec           modules;           // Vec<ModuleInfo*>

    HashMap       module_by_path;    // path StrId → ModuleId

    /* ------------------------------------------------------------------------
       Defs — SoA. Every column parallel-indexed by DefId.idx.

       Identity columns survive across reparses (DefId allocation is keyed
       off AstId, which is reparse-stable). Cached query outputs invalidate
       when the per-decl durable fingerprint changes. Per-decl slot columns
       are scanned tightly by the invalidation walker.
       ------------------------------------------------------------------------ */

    struct {
        // Identity. Stable across reparses.
        Vec   names;             // StrId
        Vec   parent_modules;    // ModuleId
        Vec   kinds;             // DefKind
        Vec   visibilities;      // Visibility
        Vec   ast_ids;           // AstId
        Vec   owner_scopes;      // ScopeId

        // Per-decl durable. Hash over each decl's slice of the durable AST.
        Vec   durable_fps;       // Fingerprint

        // Cached query outputs — all IpIndex into the intern pool.
        Vec   types;             // result of query_type_of_decl
        Vec   values;            // result of query_const_eval (consts only)
        Vec   effect_sigs;       // result of query_effect_signature (fns/handlers)

        // Per-decl query slot columns. SoA so the invalidation walker
        // iterates one kind densely; each Vec is contiguous QuerySlot bytes.
        Vec   slots_type;
        Vec   slots_signature;
        Vec   slots_is_comptime;
        Vec   slots_const_eval;
    } defs;

    /* ------------------------------------------------------------------------
       Scopes — SoA. Every column parallel-indexed by ScopeId.idx.
       ------------------------------------------------------------------------ */

    struct {
        Vec   parents;           // ScopeId
        Vec   kinds;             // ScopeKind
        Vec   owning_modules;    // ModuleId
        Vec   decl_offsets;      // Vec<uint32_t> — indexed by ScopeId.idx, plus a sentinel at [scope_count]
        Vec   decl_pool;         // Vec<DeclEntry> — flat decl storage
        Vec   slots_resolve_ref; // Per-scope query slot
    } scopes;

    // Sparse-key caches

    HashMap resolve_path;        // dotted-path StrId → ResolvePathEntry (carries DefId + embedded QuerySlot)
    HashMap comptime_call_cache; // (DefId, args-hash) → IpIndex

    // Query engine state.

    uint64_t          current_revision;
    uint64_t          request_revision;
    bool              request_pinned;
    bool              invalidation_enabled;
    Vec               query_stack;       // Vec<QueryFrame>
    atomic_bool       cancel_requested;

#ifdef ORE_DEBUG_QUERIES
    struct QueryStats query_stats[QUERY_KIND_COUNT];
#endif

    uint32_t      comptime_depth_limit;

    /* ------------------------------------------------------------------------
       Pre-interned hot names. Builtin dispatch (sizeOf, alignOf, …) reads
       these every time a builtin is referenced; interning them once at
       db_init saves a pool lookup on every reference.
       ------------------------------------------------------------------------ */

    struct {
        StrId sizeOf;
        StrId alignOf;
        StrId TypeOf;
        StrId intCast;
        StrId typeName;
    } names;
};


/* ============================================================================
   Lifecycle.
   ============================================================================ */

void db_init(struct db *s);
void db_free(struct db *s);

#endif
