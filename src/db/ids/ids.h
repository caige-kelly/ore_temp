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
typedef struct TypeId   { uint32_t idx; } TypeId;

// AstId — stable per-module identity for top-level items. Derived
// from hash((kind, name)) at parse time and survives reparses with
// the same item shape. Details + map data structure in
// `sema/modules/ast_id_map.h`. Lives here next to the other ID
// newtypes so consumers don't pull the map header transitively just
// to mention the type.
typedef struct AstId    { uint32_t v; } AstId;

// ExprId — stable per-decl identity for a body-level Expr. Decoupled
// from `NodeId` (which is position-based and shifts on text-insert-
// before edits). `local` is the position in a deterministic body-
// store walk over the owning decl's body. Editing fn A reshapes A's
// local indices; fn B's stay untouched. See sema/body/body_store.h.
//
// `local == 0` is reserved as the NONE sentinel — body stores seat
// index 0 with a NULL placeholder. Use `expr_id_is_valid` to check.
typedef struct ExprId { DefId decl; uint32_t local; } ExprId;

#define DEF_ID_INVALID    ((DefId){0})
#define SCOPE_ID_INVALID  ((ScopeId){0})
#define MODULE_ID_INVALID ((ModuleId){0})
#define BODY_ID_INVALID   ((BodyId){0})
#define AST_ID_NONE       ((AstId){0})
#define EXPR_ID_NONE      ((ExprId){.decl = DEF_ID_INVALID, .local = 0})

static inline bool def_id_is_valid(DefId id)         { return id.idx != 0; }
static inline bool scope_id_is_valid(ScopeId id)     { return id.idx != 0; }
static inline bool module_id_is_valid(ModuleId id)   { return id.idx != 0; }
static inline bool body_id_is_valid(BodyId id)       { return id.idx != 0; }
static inline bool ast_id_is_valid(AstId id)         { return id.v != 0; }
static inline bool expr_id_is_valid(ExprId id)       { return def_id_is_valid(id.decl) && id.local != 0; }

static inline bool def_id_eq(DefId a, DefId b)             { return a.idx == b.idx; }
static inline bool scope_id_eq(ScopeId a, ScopeId b)       { return a.idx == b.idx; }
static inline bool module_id_eq(ModuleId a, ModuleId b)    { return a.idx == b.idx; }
static inline bool body_id_eq(BodyId a, BodyId b)          { return a.idx == b.idx; }
static inline bool ast_id_eq(AstId a, AstId b)             { return a.v == b.v; }
static inline bool expr_id_eq(ExprId a, ExprId b)          { return def_id_eq(a.decl, b.decl) && a.local == b.local; }

// Promote 32-bit IDs to the 64-bit keys our HashMap requires. Tag the
// high half with a per-family discriminator so a ScopeId and a DefId
// with the same idx hash to different buckets if they're ever mixed in
// a debugging table — keeps accidental cross-family caching visible.
//
// Tags: 0x01=Def, 0x02=Scope, 0x03=Module, 0x04=Body, 0x05=Expr.

static inline uint64_t def_id_key(DefId id)         { return ((uint64_t)0x01 << 32) | id.idx; }
static inline uint64_t scope_id_key(ScopeId id)     { return ((uint64_t)0x02 << 32) | id.idx; }
static inline uint64_t module_id_key(ModuleId id)   { return ((uint64_t)0x03 << 32) | id.idx; }
static inline uint64_t body_id_key(BodyId id)       { return ((uint64_t)0x04 << 32) | id.idx; }

// ExprId key encoding. Two variants, picked by call site:
//   expr_id_key: plain — tag(4) | decl.idx(32) | local(28). Used for
//     single-key body-level caches (type_of_expr, const_eval, ...).
//   expr_id_ns_key: namespace-packed — drops the family tag (these
//     tables only key on ExprId) and packs a 4-bit namespace into
//     the low bits. Used for resolve_ref/path entries that key on
//     (NodeId, NS) today.
static inline uint64_t expr_id_key(ExprId id) {
    return ((uint64_t)0x05 << 60) | ((uint64_t)id.decl.idx << 28) | id.local;
}
static inline uint64_t expr_id_ns_key(ExprId id, uint32_t ns) {
    return ((uint64_t)id.decl.idx << 36) | ((uint64_t)id.local << 4) | (ns & 0xF);
}

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

// Retrieve a def's originating AST node. Two paths:
//
//   - DECL_USER / DECL_IMPORT top-level: derive via the module's
//     current top-level index keyed by name. This way the lookup
//     always finds the current revision's Bind node regardless of
//     where it has shifted in the file — the (module, name) tuple
//     is the stable handle, analogous to rust-analyzer's AstId.
//
//   - Local DECL_USER (let-binds in fn bodies / blocks) and other
//     nested kinds: `origin_expr_id` → `id_to_expr` via the owning
//     decl's body_store (R8). Populated by define_local_bind during
//     scope_index_build_module; refreshed on every revision via the
//     body_store's dep on query_module_ast.
//
//   - DECL_FIELD / DECL_VARIANT / DECL_PRIMITIVE: no AST Bind to
//     return; this function yields NULL. Per-field/variant data
//     lives on `StructSignature` / `EnumSignature`.
struct Expr*       def_origin(struct Sema* s, DefId id);

// Total entries in each table, including the slot-0 placeholder.
// Useful for iteration: `for (uint32_t i = 1; i < sema_def_count(s); i++)`.
uint32_t sema_def_count(struct Sema* s);
uint32_t sema_scope_count(struct Sema* s);
uint32_t sema_module_count(struct Sema* s);
uint32_t sema_body_count(struct Sema* s);

// db/ids/ids.h

// The opaque 64-bit integer that represents a universally unique AST Node.
typedef struct {
    uint64_t raw;
} GlobalNodeId;

// Helper to pack it
static inline GlobalNodeId global_node_id(uint32_t module_id, uint32_t local_id) {
    return (GlobalNodeId){ .raw = ((uint64_t)module_id << 32) | local_id };
}

// Helpers to unpack it
static inline uint32_t global_node_module(GlobalNodeId g) {
    return (uint32_t)(g.raw >> 32);
}

static inline uint32_t global_node_local(GlobalNodeId g) {
    return (uint32_t)(g.raw & 0xFFFFFFFF);
}

// Compare them with a single CPU instruction
static inline bool global_node_eq(GlobalNodeId a, GlobalNodeId b) {
    return a.raw == b.raw;
}

#endif // ORE_SEMA_IDS_H
