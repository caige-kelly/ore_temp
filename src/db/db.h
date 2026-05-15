#ifndef ORE_DB_H
#define ORE_DB_H

#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "./ids/ids.h"
#include "./intern_pool/intern_pool.h"
#include "./query/query.h"
#include "./request/request.h"
#include "./storage/arena.h"
#include "./storage/hashmap.h"
#include "./storage/stringpool.h"
#include "./storage/vec.h"
#include "names.inc"

/* --- Column Types --- */

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
  SCOPE_UNION,
  SCOPE_ENUM,
} ScopeKind;

typedef struct {
  StrId name;
  DefId def;
} DeclEntry;

typedef struct {
  uint32_t file_id : 16;
  uint32_t start : 24;
  uint32_t length : 24;
} __attribute__((packed)) TinySpan; // 8 bytes total

typedef struct {
  StrId path;
  DefId def;
  struct QuerySlot slot;
} ResolvePathEntry;

typedef enum {
  ID_NONE = 0,
#define X(id, name) ID_##id,
  BUILTIN_LIST(X)
#undef X
} BuiltinId;

typedef enum {
  _CNXT_START = 127,
#define X(id, name) CNXT_##id,
  CONTEXT_LIST(X)
#undef X
} ContextId;

/* --- Definition Metadata (8 bits) --- */
typedef uint8_t DefMeta;
#define META_VIS_MASK 0x03 // Bits 0-1 (Visibility)
#define META_COMPTIME 0x04 // Bit 2 (is_comptime)
#define META_SCOPED 0x08   // Bit 3
#define META_NAMED 0x10    // Bit 4
#define META_LINEAR 0x20   // Bit 5

/* --- Scope Metadata (8 bits) --- */
typedef uint8_t ScopeMeta;
#define META_SCOPE_KIND_MASK 0x07 // Bits 0-2 (ScopeKind)

/* --- The Database --- */
struct db {
  /* --- Control & Invalidation ---  */
  // [1 bit: Invalidation] [ 31 bits: Current Rev ] [ 32 bits: Request Rev ]
  alignas(64) _Atomic uint64_t rev_control;

// Bitmasks
#define REV_INVALIDATION_MASK (1ULL << 63)
#define REV_CURRENT_MASK (0x7FFFFFFFULL << 32)
#define REV_REQUEST_MASK (0xFFFFFFFFULL)

  uint32_t comptime_depth_limit;

  /* --- Cancellation --- */
  alignas(64) atomic_bool cancel_requested;

  /* --- WARM: Global Managers -------------------------------------------- */
  alignas(64) Arena arena;
  Arena request_arena;
  StringPool strings;
  InternPool intern;
  Vec query_stack; // Vec<QueryFrame>

  /* --- COLD: The Tables (SoA Headers) ----------------------------------- */

  struct {
    Vec names;       // Vec<StrId>
    Vec files;       // Vec<FileId>
    Vec durable_fps; // Vec<Fingerprint>
    Vec line_starts; // Vec<Vec<uint32_t>>

    // Contiguous block containing Spans, then Parents, then Types.
    // block = node_side_data[mod_id]
    // size  = node_counts[mod_id] * 16 bytes
    Vec node_side_data; // Vec<void*>
    Vec node_counts; // Vec<uint32_t>
    Vec ids;
    Vec asts;
    Vec trivia_tokens;
    Vec trivia_offsets;
    Vec ast_id_maps;
    Vec top_level_indices;
    Vec node_to_decls;

    // Per-module query slots
    Vec slots_ast, slots_index, slots_exports;
  } modules;

  struct {
    Vec names, kinds, ast_ids, owner_scopes;
    Vec meta; // Vec<DefMeta> - Bitpacked Def properties
    Vec durable_fps;
    Vec types, values, effect_sigs;
    Vec slots_type, slots_signature, slots_const_eval;
  } defs;

  struct {
    Vec parents;           // ScopeId
    Vec meta;              // Vec<ScopeMeta>
    Vec owning_modules;    // ModuleId
    Vec decl_offsets;      // Vec<uint32_t>
    Vec decl_pool;         // Vec<DeclEntry>
    Vec slots_resolve_ref; // Per-scope query slot
  } scopes;

  struct {
    Vec hashes;    // Vec<uint64_t>
    Vec versions;  // Vec<uint32_t> (LSP revisions)
    Vec paths;     // Vec<StrId>
    Vec texts;     // Vec<char*>
    Vec text_lens; // Vec<uint32_t>
  } sources;

  /* --- COLDER: Sparse Caches (HashMaps) --------------------------------- */
  HashMap module_by_path;
  HashMap resolve_path;
  HashMap comptime_call_cache;
};

void db_init(struct db *s);
void db_free(struct db *s);

#endif
