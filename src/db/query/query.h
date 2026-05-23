#ifndef ORE_DB_QUERY_H
#define ORE_DB_QUERY_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "../storage/vec.h"
#include "../storage/arena.h"
#include "../ids/ids.h"

struct db;

typedef enum {
    QUERY_EMPTY = 0,
    QUERY_RUNNING,
    QUERY_DONE,
    QUERY_ERROR,
} QueryState;

typedef enum {
    QUERY_TYPE_OF_DECL,
    QUERY_LAYOUT_OF_TYPE,
    QUERY_INSTANTIATE_DECL,
    QUERY_EFFECT_SIG,
    QUERY_BODY_EFFECTS,
    QUERY_MODULE_AST,
    QUERY_FILE_AST,
    QUERY_MODULE_EXPORTS,
    QUERY_MODULE_FOR_PATH,
    QUERY_TOP_LEVEL_INDEX,
    QUERY_SCOPE_FOR_NODE,
    QUERY_SCOPE_DECLS,
    QUERY_SCOPE_PARENT,
    QUERY_EFFECT_OPS_VISIBLE,
    QUERY_RESOLVE_REF,
    QUERY_RESOLVE_PATH,
    QUERY_DEF_IDENTITY,
    QUERY_NODE_TO_DECL,
    QUERY_FN_SCOPE_INDEX,
    QUERY_CONST_EVAL,
    QUERY_INFER_BODY,
    QUERY_FN_SIGNATURE,
    QUERY_STRUCT_SIGNATURE,
    QUERY_ENUM_SIGNATURE,
    QUERY_IS_COMPTIME,
    QUERY_BODY_STORE,
    QUERY_BODY_SCOPES,
    // Per-decl AST handle — keyed by packed (file_local<<32 | ast_id).
    // Fingerprint is a position-independent structural hash of one
    // top-level decl's AST subtree; sema queries depend on it (not on
    // the whole-file QUERY_FILE_AST) so a sibling edit early-cuts them.
    QUERY_DECL_AST,

    // Per-file @import refs — walks the file's AST and collects every
    // AST_EXPR_BUILTIN(name="import", path) into FileArray<ImportRef>.
    // Pure: depends on QUERY_FILE_AST. Workspace's discovery loop uses
    // this to know what to load next.
    QUERY_FILE_IMPORTS,
    // NOTE: QUERY_MODULE_FOR_PATH already exists earlier in this enum
    // (was a scaffold; Gap B wires its actual body).
} QueryKind;

#define QUERY_KIND_COUNT ((int)QUERY_FILE_IMPORTS + 1)

typedef enum {
    QUERY_BEGIN_COMPUTE,
    QUERY_BEGIN_CACHED,
    QUERY_BEGIN_CYCLE,
    QUERY_BEGIN_ERROR,
    QUERY_BEGIN_CANCELED,
} QueryBeginResult;

typedef uint64_t Fingerprint;
#define FINGERPRINT_NONE ((Fingerprint)0)

// Typed wrapper dispatch. db.recompute_dispatch[kind] is the wrapper-call
// thunk for each active QueryKind: decodes the u64 key into the wrapper's
// typed args (FileId, DefId, packed (mid,ast_id), etc.) and calls the
// wrapper. db_verify uses this to pull a recorded dep — the wrapper
// handles cache-vs-recompute internally via its own DB_QUERY_GUARD, so
// the engine never invokes a recompute directly.
//
// Populated by db_register_query_dispatch (src/db/query/dispatch.c —
// the only file that bridges engine and consumer types) at db_init time.
typedef void (*RecomputeFn)(struct db *s, uint64_t key);

typedef struct {
    uint64_t    key;     // the query key BY VALUE — see db_locate_slot
    Fingerprint dep_fp;
    QueryKind   kind;
    // Field order: the two u64s first, then the enum — so the enum's
    // tail padding packs three QueryDeps into 40 B, not 72. The dep
    // walk in db_revalidate reads all three fields per visit (AoS is
    // correct here); this is purely a packing win.
} QueryDep;

// A query's memoized slot state is split into two parallel SoA columns —
// hot and cold — at the same row index. db_revalidate (the hottest
// incremental path, run transitively per dep) reads only the hot fields;
// keeping the cold lifecycle bookkeeping out of its scan means each slot
// visit touches one cache line, not two.
//
// HOT — read/written by db_verify and db_query_begin every visit.
// db_locate_slot returns this. One cache line.
//
// Cycle detection rides on `state == QUERY_RUNNING` — verify pushes the
// frame and sets RUNNING before calling db_verify, so a recursive pull
// of the same slot hits the existing QUERY_RUNNING → QUERY_BEGIN_CYCLE
// path. There is no separate "in-progress verify" marker.
typedef struct QuerySlotHot {
    QueryState   state;
    QueryKind    kind;
    uint8_t      has_untracked_read;
    // Min (most-volatile) durability over this slot's inputs. Set at
    // db_query_succeed. Default DUR_LOW (conservative — disables the
    // durability fast-path) until a dep or noted input proves higher.
    uint8_t      durability;
    uint8_t      _pad0[2];
    Fingerprint  fingerprint;       // u64 — the memoized result fingerprint
    uint64_t     verified_rev;      // last revision proven current
    Vec         *deps;              // *Vec<QueryDep>, lazy-alloc
    // Diagnostics are NOT stored on the slot. They live in db.diags,
    // keyed by the (kind, key) analysis unit — see db.h DiagList.
} QuerySlotHot;

// COLD — read by tests (sema_rev in decl_incremental_test); production
// uses the hot fingerprint comparison directly. Same row index as the
// hot column; db_locate_slot_cold returns this.
typedef struct QuerySlotCold {
    uint64_t     computed_rev;
} QuerySlotCold;

typedef struct QueryFrame {
    QueryKind kind;
    uint64_t  key;
    // Min-durability accumulator for the running query. dur_set stays
    // false until a dep is recorded or db_query_note_input_durability
    // is called; db_query_succeed writes DUR_LOW if still unset (an
    // untracked/input query that didn't declare a tier — conservative).
    uint8_t min_input_dur;
    bool    dur_set;
    Vec* deps;   // arena-allocated Vec object (stable across query_stack realloc);
                 // backing buffer is malloc-owned by the Vec. Initialized from
                 // slot->deps so a recompute reuses the prior buffer.
#ifdef ORE_DEBUG_QUERIES
    bool has_untracked_read;
    uint64_t untracked_read_count;
#endif
} QueryFrame;

#ifdef ORE_DEBUG_QUERIES
typedef struct QueryStats {
    uint64_t begin;
    uint64_t cached_hit;
    uint64_t compute;
    uint64_t cycle;
    uint64_t error;
    uint64_t recompute_due_to_untracked;
} QueryStats;
#endif

// Lifecycle. All three functions internally resolve the slot via
// db_locate_slot(s, kind, key) — callers never hold a slot pointer across
// a body, which makes the engine safe against Vec/HashMap reallocations
// during nested query execution. Slots are zero-initialized by the
// vec_push_zero that grows their column (QUERY_EMPTY == 0, DUR_LOW == 0),
// so there is no slot-init function.
//
// db_query_succeed takes the result fingerprint as a parameter (folds in
// the previous db_query_slot_set_fingerprint step), so each phase
// boundary does exactly one db_locate_slot call.
// The query key is a BY-VALUE uint64_t. Every key fits: DefId / FileId /
// ModuleId / StrId are u32; def_identity / resolve_ref keys are packed
// u64. db_locate_slot interprets the value per kind. By-value (vs the
// former const void*) means dep keys can't dangle and dedup is canonical.
QueryBeginResult  db_query_begin(struct db *s, QueryKind kind, uint64_t key);
void              db_query_succeed(struct db *s, QueryKind kind,
                                   uint64_t key, Fingerprint fp);
void              db_query_fail(struct db *s, QueryKind kind, uint64_t key);

// Declare that the running query read an input of the given durability
// (a Durability value; uint8_t to avoid a db.h<->query.h include
// cycle). e.g. QUERY_FILE_AST reading its source text. Lowers the
// current frame's min-durability accumulator. Idempotent.
void              db_query_note_input_durability(struct db *s, uint8_t dur);

// Bump the global revision and stamp durability tier `dur` as changed
// at it. The edit/LSP layer calls this when an input of that
// durability changes; db_revalidate then early-cuts slots whose
// durability tier is unaffected. Returns the new global revision.
uint64_t          db_input_changed(struct db *s, uint8_t dur);

QueryFrame       *db_query_stack_top(struct db *s);
const char       *db_query_kind_str(QueryKind kind);

#ifdef ORE_DEBUG_QUERIES
void  db_mark_frame_untracked(struct db *s, const char *why);
void  db_dump_query_stats(struct db *s, FILE *out);
#endif

#ifdef ORE_DEBUG_QUERIES
#define DB_READ_UNTRACKED(s, expr, why) \
    (db_mark_frame_untracked((s), (why)), (expr))
#else
#define DB_READ_UNTRACKED(s, expr, why) (expr)
#endif

#define DB_QUERY_GUARD(s, kind, key, on_cached, on_cycle, on_error) \
    do { \
        QueryBeginResult __qbr = db_query_begin((s), (kind), (key)); \
        if (__qbr == QUERY_BEGIN_CACHED) return (on_cached); \
        if (__qbr == QUERY_BEGIN_CYCLE)  return (on_cycle); \
        if (__qbr == QUERY_BEGIN_ERROR)  return (on_error); \
    } while (0)

#endif // ORE_DB_QUERY_H
