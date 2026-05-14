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
#include "./request/request.h"

/* --- Column Types --- */

// 3. Packing Query Slots into "Bit-Sets"
// For simple queries like is_comptime, you are currently using a full QuerySlot. A QuerySlot usually contains a revision (32 bits) and maybe some flags.

// The Pack: If you have many "Boolean" queries (e.g., is_pure, is_exported, is_comptime), don't give them each a QuerySlot.

// Create one Vec<uint32_t> bit_flags.

// Create one "Global Validity Revision" for the whole column.

// If the module's durable_fp hasn't changed, you trust the entire bit-set. This turns thousands of QuerySlot checks into a single fingerprint check.

// typedef uint8_t DefMeta;

// // Masks and Offsets
// #define META_VIS_MASK    0x03 // Binary 00000011
// #define META_COMPTIME    0x04 // Binary 00000100 (Bit 2)
// #define META_SCOPED      0x08 // Binary 00001000 (Bit 3)
// #define META_NAMED       0x10 // Binary 00010000 (Bit 4)
// #define META_LINEAR      0x20 // Binary 00100000 (Bit 5)

typedef enum : uint8_t {
    VIS_PRIVATE = 0, VIS_PUBLIC, VIS_INTERNAL,
} Visibility;

typedef enum : uint8_t {
    SCOPE_NONE = 0, SCOPE_MODULE, SCOPE_FUNCTION, SCOPE_BLOCK,
    SCOPE_STRUCT, SCOPE_UNION, SCOPE_ENUM,
} ScopeKind;

typedef struct {
    StrId name;
    DefId def;
} DeclEntry;


// This instead of Compact Span?
typedef struct {
  uint32_t file_id : 16;
  uint32_t start   : 24;
  uint32_t length  : 24; 
} __attribute__((packed)) TinySpan; // 8 bytes total

typedef struct {
  FileId   file;
  uint32_t byte_start;
  uint32_t byte_end;
} CompactSpan;

typedef struct {
    StrId        path;
    DefId        def;
    struct QuerySlot slot;
} ResolvePathEntry;

typedef enum {
  #define X(id, name) ID_##id,
  BUILTIN_LIST(X)
  #undef X
} BuiltinId;

typedef enum {
  #define X(id, name) CNXT_##id = 128 + ID_##id,
  CONTEXT_LIST(X)
  #undef X
} ContextId;

/* --- The Database --- */ 
struct db {
  /* --- Control & Invalidation ---  */
  // [1 bit: Invalidation] [ 32 bits: Current Rev ] [ 32 bits: Request Rev ]
  alignas(64) _Atomic uint64_t rev_control;
  
  // Bitmasks
  #define REV_INVALIDATION_MASK (1ULL << 63)
  #define REV_CURRENT_MASK      (0xFFFFFFFFULL << 32)
  #define REV_REQUEST_MASK      (0xFFFFFFFFULL)

  uint32_t              comptime_depth_limit;

  /*
    change to: 
    Range (StrId),Category,Benefit
    0,STR_NONE,Sentinel for null/empty.
    1 – 127,"Built-ins (sizeOf, intCast)","Parser knows these are ""Special Functions."""
    128 – 255,"Contextual Keywords (val, ctl)",Lexer knows these might be identifiers OR keywords.
    256+,User-defined Strings,"Everything else (variable names, etc)."
  */
  // struct {
  //     StrId sizeOf, alignOf, TypeOf, intCast, typeName;
  //     StrId val, final, raw, ctl, override, named, in, scoped, linear;
  // } names;

  /* --- Cancellation --- */
  alignas(64) atomic_bool cancel_requested;

  /* --- WARM: Global Managers -------------------------------------------- */
  alignas(64) Arena     arena;
  Arena                 request_arena;
  StringPool            strings;
  InternPool            intern;
  Vec                   query_stack; // Vec<QueryFrame>

  /* --- COLD: The Tables (SoA Headers) ----------------------------------- */

  struct {
    Vec names;           // Vec<StrId>
    Vec files;           // Vec<FileId>
    Vec durable_fps;     // Vec<Fingerprint>
    
    // Metadata Headers (The Vec structs themselves stored by value)
    Vec line_starts;     // Vec<Vec<uint32_t>>
    // Instead of:

    // span_maps -> Vec<CompactSpan>

    // parent_maps -> Vec<AstNodeId>

    // You do:

    // module_side_data -> Vec<char*> (Point to a single block containing Spans, then Parents, then Types).
    Vec span_maps;       // Vec<Vec<CompactSpan>>
    Vec parent_maps;     // Vec<Vec<AstNodeId>>
    Vec type_maps;       // Vec<Vec<IpIndex>>
    
    // Per-module query slots
    Vec slots_ast, slots_index, slots_exports;
  } modules;

  struct {
      Vec names, parent_modules, kinds, visibilities, ast_ids, owner_scopes;
      Vec durable_fps;
      Vec types, values, effect_sigs;
      Vec slots_type, slots_signature, slots_is_comptime, slots_const_eval;
  } defs;

  struct {
    Vec hashes;          // Vec<uint64_t>
    Vec versions;        // Vec<uint32_t> (LSP revisions)
    Vec paths;           // Vec<StrId>
    Vec texts;           // Vec<char*>
    Vec text_lens;       // Vec<uint32_t>
  } sources;

  /* --- COLDER: Sparse Caches (HashMaps) --------------------------------- */
  HashMap module_by_path;
  HashMap resolve_path;
  HashMap comptime_call_cache;
};

void db_init(struct db *s);
void db_free(struct db *s);

#endif
