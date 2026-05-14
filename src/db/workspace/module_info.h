#ifndef ORE_DB_WORKSPACE_MODULE_INFO_H
#define ORE_DB_WORKSPACE_MODULE_INFO_H

#include <stdbool.h>
#include <stdint.h>

#include "../storage/arena.h"
#include "../storage/vec.h"
#include "../ids/ids.h"
#include "../intern_pool/intern_pool.h"   // IpIndex (TypeMap column)
#include "../query/query.h"               // QuerySlot, Fingerprint

// Forward decls. Defined in their own headers; we only need pointers here,
// which keeps module_info.h light to include and lets ASTStore / AstIdMap
// evolve without touching this file.
struct ASTStore;
struct AstIdMap;
struct Token;


/* ============================================================================
   TinySpan — 12-byte byte-range.

   Line/column are NOT stored. They're derived on demand from the source's
   line_starts table (a Vec<uint32_t> of line-start byte offsets, built
   once during lexing). Binary search line_starts for the line containing
   byte_start; column = byte_start - line_starts[line].

   The vast majority of code paths only need (file, byte_start, byte_end)
   — diagnostic publishing is the rare case that needs line/col, and pays
   the O(log lines) lookup at LSP-message cadence rather than per-node.
   ============================================================================ */

// TinySpan is in db.h


/* ============================================================================
   ModuleInfo — everything per-module.

   Lifetime: one ModuleInfo per ModuleId, allocated in db.arena (so its
   pointer never moves) and persists for the db's lifetime. Its embedded
   `arena` field owns the per-module parse output — kinds/children/values,
   the Big Four side-tables, AstIdMap, top_level_index, node_to_decl.
   Reparsing the module = arena_reset(&mod->arena), then rebuild every
   Vec from the new parse.

   Layout philosophy:

   - Durable Data (the ASTStore) is bitwise stable across whitespace
     edits. Its fingerprint (`durable_fp`) is what drives early cutoff.
   - Volatile Data (the Big Four side-tables) is rebuilt on every
     successful parse. Side-tables don't participate in the durable
     fingerprint — span shifts must NOT invalidate downstream queries.
   - The TypeMap is semi-volatile: it's parallel-indexed by AstNodeId
     and rebuilt only when `durable_fp` changes (or, with R6 per-decl
     fingerprints, when the specific decl's slice changes).

   All Big Four Vecs are flat, AstNodeId.idx-indexed. TriviaMap is the
   one with a slight twist — see its comment.
   ============================================================================ */

struct ModuleInfo {
    /* ------------------------------------------------------------------------
       Identity.
       ------------------------------------------------------------------------ */

    ModuleId    id;
    StrId       name;          // dotted name (e.g. "std.fs"), interned
    FileId      file;          // backing source (physical or virtual)

    /* ------------------------------------------------------------------------
       Storage.
       ------------------------------------------------------------------------ */

    // Owns every allocation for this module's parse output. Reset on
    // reparse. Lives by value inside ModuleInfo — the Arena struct
    // itself is in db.arena, but its chunks are malloc'd independently.
    Arena       arena;

    // Line-start byte offsets — one entry per line, in order. Built
    // during lexing; persists across reparses unless the source content
    // changes. Used to derive (line, column) from byte offsets on
    // demand (diagnostic publishing, hover position resolution).
    //
    // Lookup: binary search for the largest entry <= byte_offset.
    // The resulting index is the 0-based line number; column is
    // (byte_offset - line_starts[line]).
    Vec         line_starts;   // Vec<uint32_t>

    /* ------------------------------------------------------------------------
       Durable AST — the "what". Bitwise stable across whitespace edits.

       ASTStore owns the kinds[], children[], values[] parallel arrays
       (the Big Three in parser_rewrite.md's terms). It does NOT carry
       any source-position data — that's exclusively in the Big Four
       side-tables below.
       ------------------------------------------------------------------------ */

    struct ASTStore *ast;

    // Fingerprint of the durable AST. Recomputed at the end of each
    // successful parse by hashing the (kinds, children, values) arrays.
    // Salsa-style early cutoff: if a reparse produces the same value,
    // every downstream query memoized against the prior fingerprint
    // remains valid.
    Fingerprint durable_fp;

    /* ------------------------------------------------------------------------
       The Big Four — the "where" and "what-was-computed-here".

       Each is parallel-indexed by AstNodeId.idx. Volatile: rebuilt on
       every parse (Span/Parent/Trivia) or on durable_fp change (Type).
       ------------------------------------------------------------------------ */

    // NodeId → byte range. 12 B/entry; line/col derived via line_starts.
    Vec         span_map;        // Vec<TinySpan>

    // NodeId → parent NodeId. 4 B/entry. AST_NODE_ID_NONE at the root.
    Vec         parent_map;      // Vec<AstNodeId>

    // NodeId → trivia (comments/whitespace) attached to that node.
    //
    // Layout is flat + offsets, NOT per-node Vec. Two parallel Vecs:
    //   trivia_tokens:       one Token per trivia entry, contiguous
    //   trivia_offsets[N]:   start index into trivia_tokens for node N
    //   trivia_offsets[N+1]: end-exclusive (i.e., count = end - start)
    //   trivia_offsets[node_count]: sentinel = trivia_tokens.count
    //
    // Cost per trivia-less node: 4 bytes (an offset slot). No per-node
    // allocation, no malloc pressure, no pointer indirection on lookup.
    // Trivia tokens stay contiguous so formatter walks stay in cache.
    struct {
        Vec     tokens;          // Vec<struct Token>
        Vec     offsets;         // Vec<uint32_t> — len = node_count + 1
    } trivia_map;

    // NodeId → IpIndex. Result of type inference for each AST node.
    // Invalidated when durable_fp changes; rebuilt by the type pass.
    Vec         type_map;        // Vec<IpIndex>

    /* ------------------------------------------------------------------------
       Identity / scoping layer.
       ------------------------------------------------------------------------ */

    // AstId → AstNodeId mapping for top-level items. AstIds are
    // reparse-stable (hashed from (kind, name)), so this map is what
    // lets DefIds (allocated keyed off AstId) survive reparses without
    // pointer churn or NodeId remapping.
    struct AstIdMap *ast_id_map;

    // Per-module top-level entry list. Cheap walk: name, AstId, span,
    // visibility — everything needed to allocate a DefId without
    // committing to typecheck-shaped state. Rebuilt on parse.
    Vec         top_level_index; // Vec<TopLevelEntry>

    // NodeId → owning top-level DefId. For any body-level AstNodeId,
    // identifies which top-level decl owns it (for "what fn does this
    // cursor position belong to" and for routing per-decl invalidation).
    // Direct array index, dense per module.
    Vec         node_to_decl;    // Vec<DefId>

    /* ------------------------------------------------------------------------
       Per-module query slots.

       Inline rather than in a side-table because each module has exactly
       one of each. The invalidation walker reaches these via
       (ModuleInfo*)key.
       ------------------------------------------------------------------------ */

    QuerySlot   slot_module_ast;
    QuerySlot   slot_top_level_index;
    QuerySlot   slot_module_exports;
    QuerySlot   slot_module_def_map;
};


/* ============================================================================
   Lifecycle.

   module_info_init/free do NOT manage the ModuleInfo struct itself — that's
   owned by db.arena. They manage the contents (arena, Vecs, AstIdMap).
   Call _init once after allocating the ModuleInfo from db.arena; call _free
   only on db teardown (or never, if db.arena is being freed wholesale).

   module_info_reset wipes the per-module arena and re-initializes all
   Vec/AstIdMap contents — for reparse without re-allocating the
   ModuleInfo struct.
   ============================================================================ */

void module_info_init(struct ModuleInfo *mod, ModuleId id, StrId name, FileId file);
void module_info_reset(struct ModuleInfo *mod);
void module_info_free(struct ModuleInfo *mod);

#endif
