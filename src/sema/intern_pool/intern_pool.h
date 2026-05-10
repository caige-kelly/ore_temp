#ifndef ORE_SEMA_INTERN_POOL_H
#define ORE_SEMA_INTERN_POOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// =====================================================================
// Unified type+value intern pool. R4 from bug_of_bugs.md.
// =====================================================================
//
// Single shared identity space for types and comptime-known values.
// Inspired by zig/src/InternPool.zig — the load-bearing trick is that
// `IpIndex` covers both "this is the i32 type" and "this is the
// comptime value 42" with one u32. Equality of two IpIndex values
// means structural equality of whatever they refer to (deduped at
// `ip_get` time).
//
// The pool's reverse function `ip_key(idx)` reconstructs an `IpKey`
// from storage. This is the hot path — everything fits in 5-byte
// items + a contiguous `extra` u32 array so the reconstruction is
// cheap.
//
// What this REPLACES (eventually, via Step 3-4 migrations):
//   - Per-kind type interning hashmaps (`type_many_ptr`, `type_ptr`,
//     etc.) → all unified under `ip_get(.{ .ptr_type = ... })`.
//   - The `ConstValue` tagged union → values become IpIndex too.
//   - Bare-u32 string ids (already typified to StrId in Step 1).
//
// What this DOESN'T do (Zig has it, we defer):
//   - Sharding / per-thread locals (we're single-threaded)
//   - TrackedInst indirection (no cross-edit identity surviving)
//   - Tag-vs-Key size compression (one Tag per Key variant for now)
//   - Compaction of removed entries (mark-removed only, leak the slot)

// ---------------------------------------------------------------------
// IpIndex — the universal identity token.
// ---------------------------------------------------------------------
//
// Wrapped u32 so the compiler enforces typedness across the rest of
// the codebase. IP_NONE is the sentinel for "no value"; reserved low
// values 0..IP_RESERVED_COUNT-1 hold primitive types and common
// constants per ip_primitives.def.
typedef struct {
    uint32_t v;
} IpIndex;

#define IP_NONE ((IpIndex){UINT32_MAX})

static inline bool ip_index_is_valid(IpIndex i) { return i.v != UINT32_MAX; }
static inline bool ip_index_eq(IpIndex a, IpIndex b) { return a.v == b.v; }

// ---------------------------------------------------------------------
// Reserved indices.
// ---------------------------------------------------------------------
//
// Every primitive type and a handful of common values get fixed
// IpIndex slots populated in ip_init(). Stable across runs (modulo
// edits to ip_primitives.def) so callers can do single-u32 compares
// like `idx == IP_BOOL_TYPE`.

enum IpReservedIndex {
    // Primitive types — order matches IP_FOREACH_PRIMITIVE expansion.
    // The X-macro guarantees this stays in sync.
#define X(lower, UPPER, SIZE, ALIGN) IP_INDEX_##UPPER##_TYPE,
#include "ip_primitives.def"
#undef X

    // Common reserved values.
    IP_INDEX_BOOL_TRUE,
    IP_INDEX_BOOL_FALSE,
    IP_INDEX_VOID_VALUE,
    IP_INDEX_UNDEF_VALUE,
    IP_INDEX_ZERO_USIZE,
    IP_INDEX_ONE_USIZE,

    // Sentinel — number of reserved indices. The pool's items array
    // starts at this length after ip_init(). Anything appended after
    // is a runtime intern.
    IP_RESERVED_COUNT,
};

// Public macros for the reserved indices. One per primitive +
// common values. Pre-baked as compile-time constants so callers can
// do `if (ip_index_eq(idx, IP_BOOL_TYPE))` cheaply.
#define X(lower, UPPER, SIZE, ALIGN) \
    static const IpIndex IP_##UPPER##_TYPE = {IP_INDEX_##UPPER##_TYPE};
#include "ip_primitives.def"
#undef X

static const IpIndex IP_BOOL_TRUE   = {IP_INDEX_BOOL_TRUE};
static const IpIndex IP_BOOL_FALSE  = {IP_INDEX_BOOL_FALSE};
static const IpIndex IP_VOID_VALUE  = {IP_INDEX_VOID_VALUE};
static const IpIndex IP_UNDEF_VALUE = {IP_INDEX_UNDEF_VALUE};
static const IpIndex IP_ZERO_USIZE  = {IP_INDEX_ZERO_USIZE};
static const IpIndex IP_ONE_USIZE   = {IP_INDEX_ONE_USIZE};

// ---------------------------------------------------------------------
// Storage layer — IpItem and IpTag.
// ---------------------------------------------------------------------
//
// The pool stores items as (tag, data) pairs. `tag` says how to
// interpret `data`: sometimes inline (e.g., a small int variant where
// `data` IS the value), sometimes an offset into the variable-length
// `extra: []u32` side-table.
//
// Per-tag storage layout is documented at each tag's case in
// intern_pool.c's ip_key() reverse function. That function IS the
// schema — keep it commented thoroughly.

typedef enum {
    // Reserved-index entries. The IpIndex value itself (its position
    // in `items`) discriminates the variant; `data` is unused (set to
    // 0). Saves ~50 storage tags vs Zig's full split.
    IP_TAG_PRIMITIVE_TYPE,        // any primitive type from .def
    IP_TAG_RESERVED_VALUE,        // BOOL_TRUE/FALSE, VOID_VALUE, UNDEF, ZERO/ONE_USIZE

    // Compound types — payload in `extra`.
    IP_TAG_PTR_TYPE,              // {elem: IpIndex, is_const: u32}
    IP_TAG_MANY_PTR_TYPE,         // {elem: IpIndex, is_const: u32}
    IP_TAG_SLICE_TYPE,            // {elem: IpIndex, is_const: u32}
    IP_TAG_ARRAY_TYPE,            // {elem: IpIndex, size_lo: u32, size_hi: u32}
    IP_TAG_OPTIONAL_TYPE,         // {elem: IpIndex}
    IP_TAG_FN_TYPE,               // {ret: IpIndex, modifiers: u32, n_params: u32, params: [n_params]IpIndex}
    IP_TAG_STRUCT_TYPE,           // nominal — {zir_node_id: u32, n_fields: u32, fields: [n_fields]{name: StrId.v, type: IpIndex}}
    IP_TAG_ENUM_TYPE,             // nominal — {zir_node_id: u32, n_variants: u32, variants: [n_variants]{name: StrId.v, value_lo: u32, value_hi: u32}}

    // Structured values — payload in `extra`.
    IP_TAG_INT_VALUE,             // {type: IpIndex, value_lo: u32, value_hi: u32}
    IP_TAG_FLOAT_VALUE,           // {type: IpIndex, value_lo: u32, value_hi: u32}

    // Mark-removed sentinel. Set by ip_remove() during compile-error
    // cleanup. ip_get() and ip_key() skip these entries. The slot
    // leaks until a future compaction pass (deferred — see plan).
    IP_TAG_REMOVED,
} IpTag;

// ---------------------------------------------------------------------
// IpKey — the union covering every variant.
// ---------------------------------------------------------------------
//
// The public/abstract view of an interned thing. Callers build keys
// to look up via `ip_get(pool, key)`, and receive keys from
// `ip_key(pool, idx)` for inspection. Storage uses a more compact
// per-Tag encoding internally.

typedef enum {
    IPK_PRIMITIVE_TYPE,    // .primitive_type = which one
    IPK_RESERVED_VALUE,    // .reserved_value = which one

    IPK_PTR_TYPE,
    IPK_MANY_PTR_TYPE,
    IPK_SLICE_TYPE,
    IPK_ARRAY_TYPE,
    IPK_OPTIONAL_TYPE,
    IPK_FN_TYPE,
    IPK_STRUCT_TYPE,
    IPK_ENUM_TYPE,

    IPK_INT_VALUE,
    IPK_FLOAT_VALUE,
} IpKeyKind;

// Compact per-variant view. Variable-length arrays in fn/struct/enum
// types are pointer-to-extra (caller owns the storage during ip_get
// calls; ip_key returns pointers into the pool's `extra` array which
// stay valid as long as the pool itself does — but a subsequent
// ip_get can realloc `extra`, so don't retain ip_key results across
// pool mutations).
typedef struct {
    IpKeyKind kind;
    union {
        // Discriminator within the reserved-index range.
        enum IpReservedIndex primitive_type;
        enum IpReservedIndex reserved_value;

        struct { IpIndex elem; bool is_const; } ptr_type;
        struct { IpIndex elem; bool is_const; } many_ptr_type;
        struct { IpIndex elem; bool is_const; } slice_type;
        struct { IpIndex elem; uint64_t size; } array_type;
        struct { IpIndex elem; } optional_type;
        struct {
            IpIndex ret;
            uint32_t modifiers;
            const IpIndex *params;  // borrowed; valid until next ip_get
            size_t n_params;
        } fn_type;
        struct {
            uint32_t zir_node_id;
            // Field arrays borrowed from `extra`; valid until next ip_get.
            const uint32_t *field_names;   // [n_fields] StrId.v
            const IpIndex *field_types;    // [n_fields]
            size_t n_fields;
        } struct_type;
        struct {
            uint32_t zir_node_id;
            const uint32_t *variant_names;  // [n_variants] StrId.v
            const int64_t *variant_values;  // [n_variants]
            size_t n_variants;
        } enum_type;

        struct { IpIndex type; int64_t value; } int_value;
        struct { IpIndex type; double value; } float_value;
    };
} IpKey;

// ---------------------------------------------------------------------
// InternPool — the storage.
// ---------------------------------------------------------------------

typedef struct {
    // Items: parallel SOA-ish arrays. Tag and data are split so the
    // hot path (ip_tag, ip_key dispatch) reads only the tag array.
    IpTag *items_tag;
    uint32_t *items_data;
    size_t items_count;
    size_t items_cap;

    // Variable-length payload area. Compounds (fn types, structs)
    // store their tail data here; their `data` field is the offset
    // into this array. Grow-only.
    uint32_t *extra;
    size_t extra_count;
    size_t extra_cap;

    // Dedup map: (full_hash → bucket → IpIndex). Open-addressed.
    // Each bucket stores high-32-bits-of-hash inline so most probes
    // can skip the full ip_key reconstruction + key_eql call.
    uint64_t *buckets;       // packed: high 32 = hash_high, low 32 = idx + 1 (0 = empty)
    size_t bucket_count;     // power of two
    size_t bucket_used;
} InternPool;

// ---------------------------------------------------------------------
// Core API.
// ---------------------------------------------------------------------

// Initialize the pool with reserved low indices populated. Must be
// called before any other operation.
void ip_init(InternPool *pool);

// Free all storage. After this, the pool struct is zeroed.
void ip_free(InternPool *pool);

// Look up or insert. Returns existing IpIndex if a structurally-
// equal key already lives in the pool; otherwise allocates a new
// item, computes its index, and dedupes future calls. Never
// returns IP_NONE for a valid key.
IpIndex ip_get(InternPool *pool, IpKey key);

// Reverse: given an index, reconstruct its key. Borrowed pointers
// inside the returned key (params, field_names, etc.) are valid
// until the next ip_get call on the same pool. Returns a key with
// kind==IPK_PRIMITIVE_TYPE and primitive_type==IP_INDEX_ERROR_TYPE
// on a removed slot or out-of-range index.
IpKey ip_key(InternPool *pool, IpIndex idx);

// Cheap predicates that don't go through ip_key.
IpTag ip_tag(InternPool *pool, IpIndex idx);
bool  ip_is_type(InternPool *pool, IpIndex idx);
bool  ip_is_value(InternPool *pool, IpIndex idx);

// ---------------------------------------------------------------------
// WipContainer — two-phase construction for self-referential nominals.
// ---------------------------------------------------------------------
//
// `struct Node { next: ^Node }` needs Node's IpIndex to exist before
// Node's field types are resolved. ip_wip_struct allocates the index
// immediately (with empty fields) and returns a handle the caller
// uses to patch fields in place via ip_wip_struct_finish.
//
// Cancel path (ip_wip_struct_cancel) marks the entry removed for
// compile-error rollback.
//
// Identity (dedup key) for a nominal type is (zir_node_id, captures)
// — NOT the field set. Two struct declarations at different source
// locations are distinct types even if their field shapes match;
// two compile passes over the same source produce the same IpIndex.

typedef struct {
    IpIndex index;          // stable; patch via the offset below
    uint32_t extra_offset;  // where to write field data
} WipContainerType;

// Allocate a wip struct type. Captures arg can be NULL with n=0 for
// non-generic types. The returned WipContainerType.index can be used
// IMMEDIATELY (e.g., as an `^Self` field type) before fields are
// resolved.
WipContainerType ip_wip_struct(InternPool *pool, uint32_t zir_node_id,
                               const IpIndex *captures, size_t n_captures);

// Patch in field types and names. Field arrays are copied into the
// pool's `extra` storage at the offset reserved by ip_wip_struct.
// After this, the type is fully resolved and ip_key returns the full
// struct shape.
void ip_wip_struct_finish(InternPool *pool, WipContainerType wip,
                          const uint32_t *field_names, const IpIndex *field_types,
                          size_t n_fields);

// Mark the wip entry removed (compile-error rollback). Future
// ip_get with the same key will allocate a fresh entry.
void ip_wip_struct_cancel(InternPool *pool, WipContainerType wip);

// ---------------------------------------------------------------------
// Removal (compile-error cleanup).
// ---------------------------------------------------------------------

// Mark an entry removed. Subsequent ip_get with the same key returns
// a fresh index; subsequent ip_key on the removed index returns the
// error sentinel. The slot leaks until compaction (deferred).
void ip_remove(InternPool *pool, IpIndex idx);

// ---------------------------------------------------------------------
// Diagnostics.
// ---------------------------------------------------------------------

// Print a compact pool summary to `out` — items count, extra count,
// bucket fill ratio, breakdown by tag. Useful for `--dump-pool-stats`
// once the pool is wired into sema. Doesn't allocate.
void ip_dump_stats(InternPool *pool, FILE *out);

#endif // ORE_SEMA_INTERN_POOL_H
