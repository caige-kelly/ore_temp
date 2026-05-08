#ifndef ORE_SEMA_IDS_H
#define ORE_SEMA_IDS_H

#include <stdbool.h>
#include <stdint.h>

// Stable opaque IDs for sema-layer entities.
//
// Every entity that the rest of the compiler refers to across query/cache
// boundaries gets a stable integer ID. Pointer-keyed caches were a
// footgun: pointers move with arena resets, can dangle, and don't
// serialize. IDs survive arena lifetime and are trivially serializable
// for future incremental compilation.
//
// idx == 0 is reserved as the INVALID sentinel for every ID family.
// Tables seat slot 0 with a NULL placeholder so accessors can return
// NULL for invalid IDs without out-of-bounds reads — see ids.c.
//
// Newtype struct wrappers (not raw integers) prevent accidental
// cross-family use: passing a ScopeId where a DefId is expected is a
// compile error.

struct Sema;
struct DefInfo;
struct ScopeInfo;
struct ModuleInfo;
struct BodyInfo;

typedef struct DefId    { uint32_t idx; } DefId;
typedef struct ScopeId  { uint32_t idx; } ScopeId;
typedef struct ModuleId { uint32_t idx; } ModuleId;
typedef struct BodyId   { uint32_t idx; } BodyId;

#define DEF_ID_INVALID    ((DefId){0})
#define SCOPE_ID_INVALID  ((ScopeId){0})
#define MODULE_ID_INVALID ((ModuleId){0})
#define BODY_ID_INVALID   ((BodyId){0})

static inline bool def_id_is_valid(DefId id)         { return id.idx != 0; }
static inline bool scope_id_is_valid(ScopeId id)     { return id.idx != 0; }
static inline bool module_id_is_valid(ModuleId id)   { return id.idx != 0; }
static inline bool body_id_is_valid(BodyId id)       { return id.idx != 0; }

static inline bool def_id_eq(DefId a, DefId b)             { return a.idx == b.idx; }
static inline bool scope_id_eq(ScopeId a, ScopeId b)       { return a.idx == b.idx; }
static inline bool module_id_eq(ModuleId a, ModuleId b)    { return a.idx == b.idx; }
static inline bool body_id_eq(BodyId a, BodyId b)          { return a.idx == b.idx; }

// Promote 32-bit IDs to the 64-bit keys our HashMap requires. Tag the
// high half with a per-family discriminator so a ScopeId and a DefId
// with the same idx hash to different buckets if they're ever mixed in
// a debugging table — keeps accidental cross-family caching visible.
//
// Tags: 0x01=Def, 0x02=Scope, 0x03=Module, 0x04=Body.

static inline uint64_t def_id_key(DefId id)         { return ((uint64_t)0x01 << 32) | id.idx; }
static inline uint64_t scope_id_key(ScopeId id)     { return ((uint64_t)0x02 << 32) | id.idx; }
static inline uint64_t module_id_key(ModuleId id)   { return ((uint64_t)0x03 << 32) | id.idx; }
static inline uint64_t body_id_key(BodyId id)       { return ((uint64_t)0x04 << 32) | id.idx; }

// Initialize the four ID tables on `s`. Pushes a NULL placeholder at
// slot 0 of each so DEF_ID_INVALID, SCOPE_ID_INVALID, etc. dereference
// to NULL via the accessors below. Idempotent: safe to call once per
// Sema lifetime; subsequent calls are no-ops.
void sema_ids_init(struct Sema* s);

// Append a freshly-built info pointer to the relevant table. The
// caller owns the pointer's lifetime (typically arena-allocated by the
// owning layer). Returns the assigned ID. Tables grow append-only.
//
// These exist in ids.c rather than each layer's own file so the
// table-allocation contract lives in exactly one place. The owning
// layer constructs its info struct, populates it, then calls the
// matching `_intern` to register it.
DefId    sema_intern_def(struct Sema* s, struct DefInfo* info);
ScopeId  sema_intern_scope(struct Sema* s, struct ScopeInfo* info);
ModuleId sema_intern_module(struct Sema* s, struct ModuleInfo* info);
BodyId   sema_intern_body(struct Sema* s, struct BodyInfo* info);

// Resolve an ID to the info pointer it names. Returns NULL when:
//   - the ID is invalid (idx == 0),
//   - the table hasn't been initialized,
//   - the idx is out of range (defensive — should never happen with
//     well-formed IDs).
//
// Callers should treat NULL as "no such entity" and either propagate
// a sentinel or emit a diagnostic. Crashing on NULL is a bug —
// well-formed code paths should never produce an out-of-range ID.
struct DefInfo*    def_info(struct Sema* s, DefId id);
struct ScopeInfo*  scope_info(struct Sema* s, ScopeId id);
struct ModuleInfo* module_info(struct Sema* s, ModuleId id);
struct BodyInfo*   body_info(struct Sema* s, BodyId id);

// Total entries in each table, including the slot-0 placeholder.
// Useful for iteration: `for (uint32_t i = 1; i < sema_def_count(s); i++)`.
uint32_t sema_def_count(struct Sema* s);
uint32_t sema_scope_count(struct Sema* s);
uint32_t sema_module_count(struct Sema* s);
uint32_t sema_body_count(struct Sema* s);

#endif // ORE_SEMA_IDS_H
