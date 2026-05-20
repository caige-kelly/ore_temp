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
  VIS_PRIVATE  = 0,
  VIS_PUBLIC   = 1,
  VIS_INTERNAL = 2,
} Visibility;

// Input durability — how often an input of this kind changes (Salsa
// tiers). Numeric value = volatility rank: LOW is the most volatile
// (workspace file text, edited constantly), HIGH the most stable
// (library sources). A derived query's durability is the MIN (most
// volatile) over its inputs, so "min" == numeric min. Used purely to
// make early-cutoff cheaper: a query provably free of any input at-or-
// below its durability can skip the dependency walk entirely.
typedef enum : uint8_t {
  DUR_LOW    = 0, // workspace file text — changes on every keystroke
  DUR_MEDIUM = 1, // file->module map / module metadata
  DUR_HIGH   = 2, // library sources — effectively immutable in a session
} Durability;
#define DUR_COUNT 3

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
  AstId ast_id; // Stable parser-side identity. Consumers route through
                // db_query_def_identity(mid, ast_id) to materialize the
                // canonical DefId on demand — the slot+DefId live in
                // db.def_by_identity (HashMap) and persist across
                // db_query_module_exports re-runs, so name/decl edits
                // never re-allocate the DefId for an unchanged AstId.
                //
                // Deliberately no `def` field: keeping the DefId off
                // DeclEntry structurally prevents callers from caching
                // and reading a stale DefId across re-runs.
} DeclEntry;

// TinySpan — 8-byte packed source byte range.
//
// Field layout:
//   bits  0-15: file_id  (16 bits, ~65k files max)
//   bits 16-39: start    (24 bits, 16MB byte offset max)
//   bits 40-63: length   (24 bits, 16MB token length max — see assert in
//                          db_alloc_source)
//
// Implemented as a plain uint64_t + inline accessors
typedef uint64_t TinySpan;

#define TINYSPAN_NONE ((TinySpan)0)

#define TINYSPAN_FILE_MASK    ((TinySpan)0xFFFFu)
#define TINYSPAN_OFFSET_MASK  ((TinySpan)0xFFFFFFu)
#define TINYSPAN_START_SHIFT  16
#define TINYSPAN_LENGTH_SHIFT 40

static inline uint16_t span_file(TinySpan s) {
    return (uint16_t)(s & TINYSPAN_FILE_MASK);
}
static inline uint32_t span_start(TinySpan s) {
    return (uint32_t)((s >> TINYSPAN_START_SHIFT) & TINYSPAN_OFFSET_MASK);
}
static inline uint32_t span_length(TinySpan s) {
    return (uint32_t)((s >> TINYSPAN_LENGTH_SHIFT) & TINYSPAN_OFFSET_MASK);
}
static inline uint32_t span_end(TinySpan s) {
    return span_start(s) + span_length(s);
}

static inline TinySpan span_make(uint16_t file, uint32_t start, uint32_t length) {
    return ((TinySpan)file & TINYSPAN_FILE_MASK)
         | (((TinySpan)start  & TINYSPAN_OFFSET_MASK) << TINYSPAN_START_SHIFT)
         | (((TinySpan)length & TINYSPAN_OFFSET_MASK) << TINYSPAN_LENGTH_SHIFT);
}

// Construct from (start, end-exclusive) — the common parser pattern.
static inline TinySpan span_make_range(uint16_t file, uint32_t start, uint32_t end) {
    return span_make(file, start, end - start);
}

// Replace the file_id of an existing span, preserving start + length.
static inline TinySpan span_with_file(TinySpan s, uint16_t file) {
    return (s & ~TINYSPAN_FILE_MASK) | ((TinySpan)file & TINYSPAN_FILE_MASK);
}

typedef struct {
#define X(id, name) StrId id;
    BUILTIN_LIST(X)
    CONTEXT_LIST(X)
#undef X
} DbNames;

typedef struct {
  StrId path;
  DefId def;
  struct QuerySlot slot;
} ResolvePathEntry;

// Entry for the (ModuleId, AstId) → DefId stable-identity map. Mirrors
// ResolvePathEntry: key + cached result + embedded per-entry QuerySlot.
// Lives in db.arena (pointer-stable for the db's lifetime), referenced
// from db.def_by_identity via its packed (mid.idx << 32 | ast_id.idx)
// key. The canonical home for the DefId — module_exports re-runs do
// NOT re-allocate; they re-query and get the same DefId back.
typedef struct {
  uint64_t key;            // packed (mid.idx << 32) | ast_id.idx
  DefId def;
  struct QuerySlot slot;
} DefIdentityEntry;

// Entry for the (ScopeId, StrId) → DefId name-resolution cache. Per-
// (scope, name) precision via HashMap, mirroring DefIdentityEntry —
// salsa dep tracking is per-entry slot, so editing one decl invalidates
// only the resolutions that depended on that name's resolution result.
typedef struct {
  uint64_t key;            // packed (scope.idx << 32) | name.idx
  DefId def;
  struct QuerySlot slot;
} ResolveRefEntry;

// Per-file node-side data — a single allocation per file containing
// three parallel arrays indexed by AstNodeId. The pointers are interior
// to one contiguous block so spans/parents/types for a given file
// stay cache-coherent on AST walks. The element count for all three is
// files.node_counts[file_id].
typedef struct {
  TinySpan  *spans;    // [node_counts[mod_id]]
  AstNodeId *parents;  // [node_counts[mod_id]]
  IpIndex   *types;    // [node_counts[mod_id]]
} ModuleNodeData;

/* --- Definition Metadata (8 bits) --- */
typedef uint8_t DefMeta;
#define META_VIS_MASK 0x03 // Bits 0-1 (Visibility)
#define META_COMPTIME 0x04 // Bit 2 (is_comptime)
#define META_SCOPED 0x08   // Bit 3 (scoped effect)
#define META_NAMED 0x10    // Bit 4 (named effect/handler)
#define META_LINEAR 0x20   // Bit 5 (linear effect/handler)
#define META_ABSTRACT 0X40 // Bit 6 (public construct / private fields)
#define META_DISTINCT 0x80 // Bit 7 (distinct constructs like ints, fns)

typedef struct {
  StrId name;
  AstNodeId node;
  DefMeta meta;
  AstId ast_id;
} TopLevelEntry;


/* --- Scope Metadata (8 bits) --- */
typedef uint8_t ScopeMeta;
#define META_SCOPE_KIND_MASK 0x07 // Bits 0-2 (ScopeKind)

/* --- The Database --- */
struct db {
  /* --- Control & Invalidation ---  */
  // [1 bit: Invalidation] [ 31 bits: Current Rev ] [ 32 bits: Request Rev ]
  _Alignas(64) _Atomic uint64_t rev_control;

// Bitmasks
#define REV_INVALIDATION_MASK (1ULL << 63)
#define REV_CURRENT_MASK (0x7FFFFFFFULL << 32)
#define REV_REQUEST_MASK (0xFFFFFFFFULL)

  // Per-durability "last changed" revision. dur_last_changed[D] = the
  // global revision at which an input of durability D last changed. An
  // edit bumps the global revision AND dur_last_changed[edited input's
  // durability]. db_revalidate early-cuts a slot when no input at its
  // durability tier changed since it was last verified — without
  // walking its dependency graph at all. Initialized to the db's
  // starting revision (1) so the first revalidation is exact.
  _Atomic uint64_t dur_last_changed[DUR_COUNT];

  uint32_t comptime_depth_limit;

  /* --- Cancellation --- */
  _Alignas(64) atomic_bool cancel_requested;

  /* --- WARM: Global Managers -------------------------------------------- */
  _Alignas(64) Arena arena;
  Arena request_arena;
  StringPool strings;
  InternPool intern;
  Vec query_stack; // Vec<QueryFrame>

  // Pre-interned StrIds for hot builtin + contextual-keyword identifiers.
  // Populated at db_init from BUILTIN_LIST / CONTEXT_LIST (names.inc).
  DbNames names;

  /* --- COLD: The Tables (SoA Headers) ----------------------------------- */

  // FILES — the per-file parse unit. One row per FileId; the parse
  // query (QUERY_FILE_AST) is keyed here. Every column is file-scoped
  // (one file's parse artifacts), so editing one file reparses only
  // its row. source_id / module_id are explicit back-refs that keep
  // source, file, and module distinct id spaces (1:1 today, N:1 ready).
  struct {
    Vec ids;          // Vec<FileId> — the row's own FileId; pointer-stable
                      // QUERY_FILE_AST slot key (db_locate_slot derefs it).
    Vec source_id;    // Vec<SourceId> — backing source (text/version/hash).
    Vec module_id;    // Vec<ModuleId> — owning module (back-ref).
    Vec durable_fps;  // Vec<Fingerprint>

    // Per-file line_starts. Each inner Vec<uint32_t> holds the byte
    // offsets of line starts for that file — built by the lexer,
    // consumed by diagnostic / LSP-position / layout-column derivation.
    // Per-file isolation: reparse refills one file's slice in
    // O(lines_in_file) without touching others.
    Vec line_starts;  // Vec<Vec<uint32_t>>

    // Per-file node-side data. node_data[F] points at a contiguous
    // block containing TinySpan[], AstNodeId[], IpIndex[] all sized to
    // node_counts[F]. See the ModuleNodeData typedef above.
    Vec node_data;    // Vec<ModuleNodeData>
    Vec node_counts;  // Vec<uint32_t>

    // Per-file durable arena. Hosts that file's ASTStore + the
    // flattened node-data block. arena_reset at the top of
    // QUERY_FILE_AST before a reparse (O(1) per-file isolation),
    // arena_free in db_ids_free.
    Vec arenas;       // Vec<Arena>

    Vec asts;         // Vec<struct ASTStore *>
    Vec trivia_tokens;
    Vec trivia_offsets;
    Vec ast_id_maps;
    Vec top_level_indices;

    // Per-file QUERY_FILE_AST slot. SoA column so the revalidation
    // walker iterates one kind densely. Slot pointers are NOT cached
    // by callers — db_locate_slot re-resolves on every db_query_begin
    // via (kind, FileId) per the kind/key-centric query API.
    Vec slots_ast;
  } files;

  // MODULES — a thin aggregation over a set of files. Holds only
  // genuinely module-scoped state; its contents (top-level index /
  // exports) are DERIVED queries that aggregate over the module's
  // files (each recording a dep on that file's QUERY_FILE_AST, so an
  // edit to one file early-cuts the others).
  struct {
    Vec ids;          // Vec<ModuleId> — pointer-stable module-query slot key.
    Vec names;        // Vec<StrId>

    // The module's file list, as a flat pool + per-module [start,end)
    // offsets (same idiom as scopes.decl_offsets/decl_pool). Module M's
    // files = file_pool[file_offsets[M] .. file_offsets[M+1]). One file
    // per module at 1:1; the aggregation queries iterate this slice so
    // the N>1 path is the same code.
    Vec file_offsets; // Vec<uint32_t> — count == module_count + 1
    Vec file_pool;    // Vec<FileId>

    // Lazily-allocated per-module scope ids. SCOPE_ID_INVALID until
    // db_query_module_exports runs for the first time. Internal scope
    // collects every decl (public + private); export scope mirrors
    // only the public subset (importers query export to stay stable
    // across edits to private decls).
    Vec exports;          // Vec<ScopeId>
    Vec internal_scopes;  // Vec<ScopeId>

    Vec slots_index, slots_exports;
  } modules;

  struct {
    Vec names;           // Vec<StrId>
    Vec kinds;           // Vec<DefKind>
    Vec ast_ids;         // Vec<AstId>
    Vec owner_scopes;    // Vec<ScopeId>
    Vec parent_modules;  // Vec<ModuleId> — direct column for record_ast_dep_for_def's hot path
    Vec meta;            // Vec<DefMeta> — bitpacked visibility + bool flags
    Vec durable_fps;     // Vec<Fingerprint>
    Vec types;           // Vec<IpIndex>
    Vec values;          // Vec<IpIndex>
    Vec effect_sigs;     // Vec<IpIndex>
    // Per-def query slots for downstream computations. The IDENTITY
    // slot is NOT in db.defs — it lives embedded in each
    // DefIdentityEntry inside the db.def_by_identity HashMap, keyed
    // by (mid, ast_id) instead of by DefId. That's where DefIds are
    // canonically materialized and where stable identity is enforced
    // across module_exports re-runs.
    Vec slots_type, slots_signature, slots_const_eval;
  } defs;

  struct {
    Vec parents;           // ScopeId
    Vec meta;              // Vec<ScopeMeta>
    Vec owning_modules;    // ModuleId
    Vec decl_offsets; // Vec<uint32_t>
    Vec decl_pool;    // Vec<DeclEntry>
    // (Per-(scope, name) name-resolution slot lives embedded in
    //  ResolveRefEntry in db.resolve_ref_cache — keyed by HashMap
    //  rather than a Vec<QuerySlot> indexed by ScopeId, because
    //  many distinct names are resolved per scope.)
  } scopes;

  struct {
    Vec hashes;    // Vec<uint64_t>
    Vec versions;  // Vec<uint32_t> (LSP revisions)
    Vec paths;     // Vec<StrId>
    Vec texts;     // Vec<char*>
    Vec text_lens; // Vec<uint32_t>
    Vec durability; // Vec<Durability> — LOW (workspace) / HIGH (library)
  } sources;

  /* --- COLDER: Sparse Caches (HashMaps) --------------------------------- */
  HashMap module_by_path;
  HashMap resolve_path;
  HashMap def_by_identity;     // (mid.idx << 32 | ast_id.idx) → DefIdentityEntry*
  HashMap resolve_ref_cache;   // (scope.idx << 32 | name.idx) → ResolveRefEntry*
  HashMap comptime_call_cache;
};

void db_init(struct db *s);
void db_free(struct db *s);

#endif
