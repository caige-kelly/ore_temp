#ifndef ORE_DB_WORKSPACE_MODULE_INFO_H
#define ORE_DB_WORKSPACE_MODULE_INFO_H

#include <stdbool.h>
#include <stdint.h>

#include "../storage/arena.h"
#include "../storage/vec.h"
#include "../ids/ids.h"
#include "../intern_pool/intern_pool.h"   // IpIndex (TypeMap column)
#include "../query/query.h"               // QuerySlot, Fingerprint

// Forward decls. ASTStore + AstIdMap evolve in their own headers; we
// only need pointers here so module_info.h stays light to include.
struct ASTStore;
struct AstIdMap;



/* ============================================================================
   ModuleInfo — transient parser-output builder.

   Lifetime: ModuleInfo is a STACK TEMPORARY created by the parser-driving
   query body (db_query_module_ast). Once the parse completes, its
   fields are copied out into the SoA columns on db.modules; the
   temporary is then discarded.

   The durable storage is db.modules (SoA columns). This struct exists
   only because the parser was written against a "writes to a single
   struct" pattern. When the parser is rewritten to write directly to
   the SoA columns, this struct can be deleted entirely.

   Layout philosophy:

   - Durable Data (the ASTStore) is bitwise stable across whitespace
     edits. Its fingerprint (`durable_fp`) is what drives early cutoff.
   - Volatile Data (the Big Four side-tables) is rebuilt on every
     successful parse. Side-tables don't participate in the durable
     fingerprint — span shifts must NOT invalidate downstream queries.
   - The TypeMap is semi-volatile: it's parallel-indexed by AstNodeId
     and rebuilt only when `durable_fp` changes.

   All Big Four Vecs are flat, AstNodeId.idx-indexed.

   Per-module query slots are NOT stored here — they live in SoA
   columns on db.modules (slots_ast, slots_index, slots_exports) and
   are addressed via db_locate_slot(kind, &ModuleId).
   ============================================================================ */

struct ModuleInfo {
    /* ------------------------------------------------------------------------
       Identity.
       ------------------------------------------------------------------------ */

    ModuleId    id;
    StrId       name;          // dotted name (e.g. "std.fs"), interned
    FileId      file;          // backing source (physical or virtual)

    /* ------------------------------------------------------------------------
       Per-parse arena. Owns every allocation for the parse output (AST,
       Big Four side-tables, AstIdMap buckets). Reset on reparse via
       arena_reset; freed wholesale when the ModuleInfo temporary goes
       out of scope (module_info_free).
       ------------------------------------------------------------------------ */

    Arena       arena;

    // Line-start byte offsets — one entry per line, in order. Built
    // during lexing; persists across reparses unless source changes.
    // Used to derive (line, column) from byte offsets on demand.
    Vec         line_starts;   // Vec<uint32_t>

    /* ------------------------------------------------------------------------
       Durable AST (the "what" — bitwise stable across whitespace edits).
       ------------------------------------------------------------------------ */

    struct ASTStore *ast;

    // Fingerprint of the durable AST. Recomputed at the end of each
    // successful parse by hashing the durable arrays. Salsa-style early
    // cutoff: if a reparse produces the same value, every downstream
    // query memoized against the prior fingerprint stays valid.
    Fingerprint durable_fp;

    /* ------------------------------------------------------------------------
       Big Four side-tables (parallel arrays indexed by AstNodeId).
       Volatile: rebuilt on every parse (Span/Parent/Trivia) or on
       durable_fp change (Type).
       ------------------------------------------------------------------------ */

    // NodeId → byte range. line/col derived via line_starts.
    Vec         span_map;        // Vec<TinySpan>

    // NodeId → parent NodeId. AST_NODE_ID_NONE at the root.
    Vec         parent_map;      // Vec<AstNodeId>

    // NodeId → trivia (comments/whitespace). Flat + offsets:
    //   trivia_offsets[N]:   start index into trivia_tokens for node N
    //   trivia_offsets[N+1]: end-exclusive (count = end - start)
    //   trivia_offsets[node_count]: sentinel = trivia_tokens.count
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
    // reparse-stable (hashed from (kind, name)) so DefIds allocated
    // keyed off AstId survive reparses without pointer churn.
    struct AstIdMap *ast_id_map;

    // Per-module top-level entry list. Cheap walk: name, AstId, span,
    // visibility — everything needed to allocate a DefId without
    // committing to typecheck-shaped state. Rebuilt on parse.
    Vec         top_level_index; // Vec<TopLevelEntry>

    // NodeId → owning top-level DefId. For any body-level AstNodeId,
    // identifies which top-level decl owns it (cursor "what fn am I in?"
    // and per-decl invalidation routing). Direct array index.
    Vec         node_to_decl;    // Vec<DefId>
};


/* ============================================================================
   Lifecycle.

   module_info_init/reset/free manage the contents (arena, Vecs,
   AstIdMap). The struct itself is the caller's stack temporary; no
   alloc/free of the struct itself.
   ============================================================================ */

void module_info_init(struct ModuleInfo *mod, ModuleId id, StrId name, FileId file);
void module_info_reset(struct ModuleInfo *mod);
void module_info_free(struct ModuleInfo *mod);

#endif
