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
} QueryKind;

#define QUERY_KIND_COUNT ((int)QUERY_BODY_STORE + 1)

typedef enum {
    QUERY_BEGIN_COMPUTE,
    QUERY_BEGIN_CACHED,
    QUERY_BEGIN_CYCLE,
    QUERY_BEGIN_ERROR,
    QUERY_BEGIN_CANCELED,
} QueryBeginResult;

typedef uint64_t Fingerprint;
#define FINGERPRINT_NONE ((Fingerprint)0)

typedef struct {
    QueryKind kind;
    const void* key;
    Fingerprint dep_fp;
} QueryDep;

typedef struct QuerySlot {
    QueryState   state;             // u8
    uint8_t      _pad0;
    QueryKind    kind;              // u16
    Fingerprint  fingerprint;       // u64
    Fingerprint  last_fingerprint;  // u64 — for changed_rev backdating
    uint64_t     computed_rev;
    uint64_t     verified_rev;
    uint64_t     changed_rev;       // Salsa backdating
    uint64_t     last_accessed_rev; // reserved for future eviction
    Vec         *deps;              // *Vec<QueryDep>, lazy-alloc
    Vec         *diags;             // *Vec<Diag>, lazy-alloc, lives in diag_arena
    Arena       *diag_arena;        // lazy-alloc; owns diags' backing memory
    size_t       diag_error_count;
    bool         has_untracked_read;
    // Transient: set while this slot is on the db_revalidate descent
    // stack. Re-entry => a dependency-graph cycle reached mid-verify;
    // db_revalidate then bails to RECOMPUTE (the compute path's
    // QUERY_RUNNING->CYCLE handling resolves the real cycle). Always
    // cleared on unwind by the db_revalidate wrapper.
    bool         revalidating;
    // Min (most-volatile) durability over this slot's inputs. Set at
    // db_query_succeed. Default DUR_LOW (conservative — disables the
    // durability fast-path, leaving exact dep-walk behavior) until a
    // dependency or noted input proves a higher tier.
    uint8_t      durability;
} QuerySlot;

typedef struct QueryFrame {
    QueryKind kind;
    const void* key;
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
    uint64_t compute_value_changed;
    uint64_t compute_value_stable;
} QueryStats;
#endif

// Initialize a slot in-place. Only public function that still touches a
// QuerySlot* directly — callers invoke it at slot-home allocation time
// (db_alloc_def / db_alloc_scope), when the slot's storage is known
// stable for the duration of the call.
void              db_query_slot_init(QuerySlot *slot, QueryKind kind);

// Lifecycle. All three functions internally resolve the slot via
// db_locate_slot(s, kind, key) — callers never hold a QuerySlot* across
// a body, which makes the engine safe against Vec/HashMap reallocations
// during nested query execution.
//
// db_query_succeed takes the result fingerprint as a parameter (folds in
// the previous db_query_slot_set_fingerprint step), so each phase
// boundary does exactly one db_locate_slot call.
QueryBeginResult  db_query_begin(struct db *s, QueryKind kind, const void *key);
void              db_query_succeed(struct db *s, QueryKind kind,
                                   const void *key, Fingerprint fp);
void              db_query_fail(struct db *s, QueryKind kind, const void *key);

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
