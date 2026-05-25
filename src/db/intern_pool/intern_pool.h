#ifndef ORE_DB_INTERN_POOL_H
#define ORE_DB_INTERN_POOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "../ids/ids.h"           // StrId, DefId — typed payload fields
#include "../../support/data_structure/arena.h"

// =====================================================================
// Unified type+value intern pool. Inspired by zig/src/InternPool.zig.
//
// One IpIndex covers everything that has identity in the type system:
// primitive types, compound types (ptr/slice/optional/array/fn/struct/
// enum), comptime values (int/float/reserved), and effect rows. Identity
// equality is u32 compare; structural dedup happens at ip_get time.
//
// Storage layout (see InternPool struct below):
//
//   - items_tag[]: 1 byte per item, flat realloc'd. The hot scan target;
//     ip_tag and the bucket-probe filter only read this array.
//   - items_data[]: 4 bytes per item, flat realloc'd. Meaning depends on
//     the item's tag:
//
//       * IP_TAG_PRIMITIVE_TYPE / IP_TAG_RESERVED_VALUE: unused; the
//         variant is recovered from the IpIndex value itself.
//
//       * Inline-encoded tags (the ptr/slice/optional family below):
//         items_data IS the elem IpIndex.v. No extra_arena access on
//         ip_key. This collapses the most common compound types
//         (pointers, slices, optionals) to a single cache line per
//         lookup.
//
//       * Arena-stored tags (array, fn, struct, enum, effect_row, int,
//         float): items_data is a byte offset into extra_arena, where
//         the typed payload struct lives.
//
//   - extra_arena: a chained Arena owning the variable-length payloads.
//     Chunks never move; pointers returned by arena_alloc_raw are stable
//     for the pool's lifetime. This is what makes the borrowed pointers
//     in ip_key()'s result valid until ip_free.
//
//   - buckets: open-addressed dedup map. Each bucket packs the high 32
//     bits of the hash with (idx+1) — 0 means empty. The hash_high
//     filter lets probes skip most ip_key_eql calls.
//
// Reserved indices (0 .. IP_RESERVED_COUNT-1) are populated at ip_init
// time at fixed positions. The IpIndex value IS the IpReservedIndex enum
// value for these slots, so lookups bypass the bucket map entirely.
// Two tiers of reserved indices:
//
//   - Primitives + common values: one slot each, no payload (variant
//     from index).
//   - Reserved compounds (^const u8, ^void, ^const void, []const u8,
//     empty effect row): one slot + a payload pre-allocated in
//     extra_arena, with a registered bucket entry so user ip_get calls
//     that construct the same shape hit the reserved slot.
//
// Removal: ip_remove marks items_tag[idx] = IP_TAG_REMOVED; bucket entry
// stays, probes skip past via the tag check. Compaction is deferred (see
// ip_compact stub).
// =====================================================================


// ---------------------------------------------------------------------
// IpIndex — the universal identity token.
// ---------------------------------------------------------------------

typedef struct {
    uint32_t v;
} IpIndex;

#define IP_NONE ((IpIndex){UINT32_MAX})

static inline bool ip_index_is_valid(IpIndex i) { return i.v != UINT32_MAX; }
static inline bool ip_index_eq(IpIndex a, IpIndex b) { return a.v == b.v; }


// ---------------------------------------------------------------------
// Reserved indices.
//
// Every primitive type, common value, and reserved compound gets a
// fixed IpIndex slot populated in ip_init(). The X-macro over
// ip_primitives.def guarantees ordering matches the static_keys init
// array.
// ---------------------------------------------------------------------

enum IpReservedIndex {
    // Primitive types — order matches IP_FOREACH_PRIMITIVE expansion.
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

    // Reserved compound types. Each is a pre-allocated payload in
    // extra_arena with a registered bucket entry, so subsequent
    // ip_get calls for the same shape return the reserved IpIndex.
    // Single-u32 identity check via ip_index_eq.
    IP_INDEX_C_STRING_TYPE,           // ^const u8
    IP_INDEX_OPAQUE_PTR_TYPE,         // ^void
    IP_INDEX_CONST_OPAQUE_PTR_TYPE,   // ^const void
    IP_INDEX_STRING_SLICE_TYPE,       // []const u8

    // Reserved compound values.
    IP_INDEX_EMPTY_EFFECT_ROW,        // <>

    // Sentinel — number of reserved indices. ip_init initializes the
    // pool with items_count == IP_RESERVED_COUNT; anything appended
    // after is a runtime intern.
    IP_RESERVED_COUNT,
};

// Per-primitive macro IpIndex constants — IP_BOOL_TYPE, IP_U8_TYPE, …
#define X(lower, UPPER, SIZE, ALIGN) \
    static const IpIndex IP_##UPPER##_TYPE = {IP_INDEX_##UPPER##_TYPE};
#include "ip_primitives.def"
#undef X

// Reserved-value constants.
static const IpIndex IP_BOOL_TRUE   = {IP_INDEX_BOOL_TRUE};
static const IpIndex IP_BOOL_FALSE  = {IP_INDEX_BOOL_FALSE};
static const IpIndex IP_VOID_VALUE  = {IP_INDEX_VOID_VALUE};
static const IpIndex IP_UNDEF_VALUE = {IP_INDEX_UNDEF_VALUE};
static const IpIndex IP_ZERO_USIZE  = {IP_INDEX_ZERO_USIZE};
static const IpIndex IP_ONE_USIZE   = {IP_INDEX_ONE_USIZE};

// Reserved-compound constants — single-u32 identity check.
static const IpIndex IP_C_STRING_TYPE         = {IP_INDEX_C_STRING_TYPE};
static const IpIndex IP_OPAQUE_PTR_TYPE       = {IP_INDEX_OPAQUE_PTR_TYPE};
static const IpIndex IP_CONST_OPAQUE_PTR_TYPE = {IP_INDEX_CONST_OPAQUE_PTR_TYPE};
static const IpIndex IP_STRING_SLICE_TYPE     = {IP_INDEX_STRING_SLICE_TYPE};
static const IpIndex IP_EMPTY_EFFECT_ROW      = {IP_INDEX_EMPTY_EFFECT_ROW};


// ---------------------------------------------------------------------
// Storage layer — IpTag.
// ---------------------------------------------------------------------
//
// items_data interpretation depends on tag:
//   - PRIMITIVE_TYPE / RESERVED_VALUE: unused
//   - Inline-encoded compounds (PTR / PTR_CONST / MANY_PTR /
//     MANY_PTR_CONST / SLICE / SLICE_CONST / OPTIONAL): items_data ==
//     elem IpIndex.v
//   - Arena-stored compounds (everything else): items_data is a byte
//     offset into extra_arena; the typed payload struct (defined in
//     intern_pool.c) lives there.
//
// REMOVED is a tombstone: probes skip past via the tag check; the
// bucket entry is left in place.

typedef enum {
    IP_TAG_PRIMITIVE_TYPE,        // items_data unused
    IP_TAG_RESERVED_VALUE,        // items_data unused

    // ---- Inline-encoded — items_data == elem.v. Zero arena access.
    IP_TAG_PTR_TYPE,              // ^T
    IP_TAG_PTR_CONST_TYPE,        // ^const T
    IP_TAG_MANY_PTR_TYPE,         // [^]T
    IP_TAG_MANY_PTR_CONST_TYPE,   // [^]const T
    IP_TAG_SLICE_TYPE,            // []T
    IP_TAG_SLICE_CONST_TYPE,      // []const T
    IP_TAG_OPTIONAL_TYPE,         // ?T

    // ---- Arena-stored — items_data is extra_arena byte offset.
    IP_TAG_ARRAY_TYPE,            // [N]T
    IP_TAG_FN_TYPE,               // fn(...) -> T
    IP_TAG_STRUCT_TYPE,           // nominal struct (zir-keyed identity)
    IP_TAG_ENUM_TYPE,             // nominal enum  (zir-keyed identity)
    IP_TAG_EFFECT_ROW,            // <E1, E2, …> — sorted DefId set

    IP_TAG_INT_VALUE,
    IP_TAG_FLOAT_VALUE,

    // File-as-namespace struct type. Identity = (nsid, field set).
    // Stores (StrId field_name, DefId field_def) pairs. Field TYPES are
    // resolved lazily via db_query_type_of_def(field_def) on access —
    // matches Zig's Namespace.owner_type (sema looks up the Nav, then
    // analyzes the Nav's type on demand). Sema's dot-access on this
    // tag falls back to the same struct-field-lookup machinery as
    // IP_TAG_STRUCT_TYPE.
    IP_TAG_NAMESPACE_TYPE,

    IP_TAG_REMOVED,

    // Sentinel — total number of IpTag variants. Used to size per-tag
    // tables (ip_dump_stats, future telemetry). Adding a new tag must
    // place it BEFORE IP_TAG_REMOVED to avoid disturbing the tombstone
    // sentinel's stable position.
    IP_TAG_COUNT,
} IpTag;


// ---------------------------------------------------------------------
// IpKey — the public/abstract key.
//
// Callers build IpKeys to look up via ip_get(); ip_key() returns one
// when given an IpIndex. Variable-length fields (fn params, struct
// fields, enum variant names/values, effect-row effects) are pointers
// into extra_arena — STABLE for the pool's lifetime.
// ---------------------------------------------------------------------

typedef enum {
    IPK_PRIMITIVE_TYPE,
    IPK_RESERVED_VALUE,

    IPK_PTR_TYPE,
    IPK_MANY_PTR_TYPE,
    IPK_SLICE_TYPE,
    IPK_OPTIONAL_TYPE,
    IPK_ARRAY_TYPE,
    IPK_FN_TYPE,
    IPK_STRUCT_TYPE,
    IPK_ENUM_TYPE,
    IPK_EFFECT_ROW,

    IPK_INT_VALUE,
    IPK_FLOAT_VALUE,

    IPK_NAMESPACE_TYPE, // file-as-namespace struct type — payload is (nsid, field names, field DefIds)
} IpKeyKind;

typedef struct {
    IpKeyKind kind;
    union {
        // Discriminator within the reserved-index range.
        enum IpReservedIndex primitive_type;
        enum IpReservedIndex reserved_value;

        // is_const is folded into the storage tag at intern time;
        // callers use the unified IpKey shape.
        struct { IpIndex elem; bool is_const; } ptr_type;
        struct { IpIndex elem; bool is_const; } many_ptr_type;
        struct { IpIndex elem; bool is_const; } slice_type;
        struct { IpIndex elem; }                optional_type;
        struct { IpIndex elem; uint64_t size; } array_type;

        struct {
            IpIndex        ret;
            uint32_t       modifiers;
            const IpIndex *params;     // borrowed; valid for pool lifetime
            size_t         n_params;
        } fn_type;

        struct {
            uint32_t       zir_node_id;
            const StrId   *field_names;   // borrowed; pool-lifetime
            const IpIndex *field_types;   // borrowed; pool-lifetime
            size_t         n_fields;
        } struct_type;

        struct {
            uint32_t       zir_node_id;
            const StrId   *variant_names;  // borrowed; pool-lifetime
            const int64_t *variant_values; // borrowed; pool-lifetime
            size_t         n_variants;
        } enum_type;

        struct {
            const DefId   *effects;   // sorted by .idx ascending; borrowed; pool-lifetime
            size_t         n_effects;
        } effect_row;

        struct { IpIndex type; int64_t value; } int_value;
        struct { IpIndex type; double  value; } float_value;

        // File-as-namespace struct type. Identity = (nsid, full field
        // set). Field TYPES are not stored here — they're resolved
        // lazily via db_query_type_of_def(field_defs[i]) at field-access
        // time. Matches Zig's Namespace.owner_type.
        struct {
            NamespaceId       nsid;
            const StrId   *field_names; // borrowed; pool-lifetime
            const DefId   *field_defs;  // borrowed; pool-lifetime
            size_t         n_fields;
        } namespace_type;
    };

    // Borrowed-payload lifetime guard. When a key's variable-length
    // payload (fn params, struct fields, enum variant names/values,
    // effect-row effects) is borrowed from an Arena that gets reset —
    // notably db.request_arena, reset at every request boundary — the
    // builder stamps that arena and its current generation here. ip_get
    // asserts (debug builds) the generation still matches, catching an
    // IpKey consumed after the arena was reset out from under it (e.g.
    // held across db_request_end), whose borrowed arrays now dangle.
    //
    // src_arena == NULL (the zero-initialized default) means no borrowed
    // payload — primitive / inline / reserved keys, or payloads that
    // outlive the pool — and the check is skipped. Keys built with a
    // designated initializer that omits these fields are NULL-stamped
    // automatically, so non-borrowing call sites need no change.
    const Arena *src_arena;
    unsigned     src_gen;
} IpKey;


// ---------------------------------------------------------------------
// InternPool — the storage.
// ---------------------------------------------------------------------

typedef struct {
    // Items: SoA. Flat realloc'd; indexed by IpIndex.v.
    IpTag    *items_tag;       // 1 byte per item
    uint32_t *items_data;      // 4 bytes per item — meaning depends on tag
    size_t    items_count;
    size_t    items_cap;

    // Variable-length payloads. Chained arena: chunks never move, so
    // pointers returned by arena_alloc_raw are stable for the pool's
    // lifetime.
    Arena     extra_arena;

    // Open-addressed dedup map. Each bucket packs (hash_high << 32) |
    // (idx + 1); 0 means empty. Reserved indices are registered for
    // compounds (so user ip_get hits them) but NOT for bare primitives
    // / reserved values (those take the fast path in ip_get).
    uint64_t *buckets;
    size_t    bucket_count;    // power of two
    size_t    bucket_used;
} InternPool;


// ---------------------------------------------------------------------
// Core API.
// ---------------------------------------------------------------------

// Initialize the pool with reserved low indices populated. Must be
// called before any other operation. Uses default sizing (256 initial
// buckets, 4096-byte initial extra_arena chunk).
void ip_init(InternPool *pool);

// Pre-sized init for workspaces with known scale. `initial_buckets`
// must be a power of two (and >= 16); `extra_chunk_size` is the
// extra_arena's first-chunk capacity (must be >= 64). Avoids early
// bucket grows and arena chunk grows on large workspaces.
void ip_init_with(InternPool *pool, size_t initial_buckets,
                  size_t extra_chunk_size);

// Reset the pool: drop all user-interned items, re-populate reserved
// indices. Reuses the existing storage allocations. After ip_clear,
// the pool is in the same state as a fresh ip_init.
void ip_clear(InternPool *pool);

// Free all storage. After this, the pool struct is zeroed.
void ip_free(InternPool *pool);

// Look up or insert. Returns existing IpIndex if a structurally-equal
// key already lives in the pool; otherwise allocates a new item,
// computes its index, and dedupes future calls. Never returns IP_NONE
// for a valid key.
IpIndex ip_get(InternPool *pool, IpKey key);

// Reverse: given an index, reconstruct its key. Borrowed pointers
// inside the returned key (params, field arrays, effect arrays) point
// into extra_arena and are STABLE for the pool's lifetime — they
// remain valid until ip_free.
//
// Returns a key with kind==IPK_PRIMITIVE_TYPE and primitive_type==
// IP_INDEX_ERROR_TYPE on a removed slot or out-of-range index.
IpKey ip_key(InternPool *pool, IpIndex idx);

// Cheap predicates that don't go through ip_key.
IpTag ip_tag(InternPool *pool, IpIndex idx);
bool  ip_is_type(InternPool *pool, IpIndex idx);
bool  ip_is_value(InternPool *pool, IpIndex idx);


// ---------------------------------------------------------------------
// WipContainer — two-phase construction for self-referential nominals.
// ---------------------------------------------------------------------
//
// `struct Node { next: ^Node }` and `fn dispatch(self: ^Self) -> Self`
// need their own IpIndex to exist before their inner field/param types
// are resolved. The wip API reserves the IpIndex up front and returns a
// handle the caller patches via _finish.
//
// No placeholder payload is allocated: the wip entry's items_data holds
// a sentinel until _finish encodes the real payload, so nothing leaks.
// While un-encoded, the entry is deliberately NOT registered in the
// dedup bucket map (a probe / buckets_grow rehash cannot reconstruct
// it) — both _finish functions register it once items_data is real.
//
// Identity (dedup key):
//   - structs/enums: (zir_node_id, captures) — NOT the field/variant
//     set. Two struct declarations at different source locations are
//     distinct types even if their shapes match.
//   - fn types: identity is structural (ret + modifiers + params), but
//     wip exists for the *self-referential* case where ret/params
//     can't be resolved until the wip's IpIndex is known.
//
// Concurrency / re-entrancy: _finish allocates the payload FRESH in
// extra_arena (never patching an earlier tail), which makes it safe to
// call ip_get between ip_wip_* and ip_wip_*_finish.

typedef struct {
    IpIndex  index;             // stable; usable immediately
    uint32_t reserved;          // wip-private state: ip_wip_struct stashes
                                // zir_node_id here for _finish; 0 for fn.
} WipContainerType;

// Allocate a wip struct type. `captures`/`n_captures` are the comptime-
// arg values for instantiations of a struct-returning comptime fn
// (e.g., `Vec(i32)` vs `Vec(f32)` — same source-level decl, distinct
// nominal IpIndex values keyed by (zir_node_id, captures)). For ordinary
// non-instantiated structs pass NULL/0. The returned
// WipContainerType.index can be used IMMEDIATELY (e.g., as an `^Self`
// field type) before fields are resolved.
WipContainerType ip_wip_struct(InternPool *pool, uint32_t zir_node_id,
                               const IpIndex *captures, size_t n_captures);

// Patch in field names and types. Allocates a fresh payload in
// extra_arena and re-points items_data[wip.index.v] at it. Safe to
// call regardless of intervening ip_get calls.
void ip_wip_struct_finish(InternPool *pool, WipContainerType wip,
                          const StrId   *field_names,
                          const IpIndex *field_types, size_t n_fields);

// Mark the wip entry removed (compile-error rollback).
void ip_wip_struct_cancel(InternPool *pool, WipContainerType wip);

// Same API surface, for self-referential function types.
WipContainerType ip_wip_fn_type(InternPool *pool, uint32_t modifiers,
                                size_t n_params);

void ip_wip_fn_finish(InternPool *pool, WipContainerType wip,
                      IpIndex ret, uint32_t modifiers,
                      const IpIndex *params, size_t n_params);

void ip_wip_fn_cancel(InternPool *pool, WipContainerType wip);


// ---------------------------------------------------------------------
// Removal & compaction.
// ---------------------------------------------------------------------

// Mark an entry removed. Subsequent ip_get with the same key returns
// a fresh index; subsequent ip_key on the removed index returns the
// error sentinel. The slot leaks until compaction.
void ip_remove(InternPool *pool, IpIndex idx);

// Compaction — rebuild items/data/extra_arena/buckets from scratch
// to reclaim removed slots. NOT YET IMPLEMENTED; returns false. Wire
// the trigger criterion (removed_count > items_count / 4) at call
// sites so the eventual implementation is mechanical.
bool ip_compact(InternPool *pool);


// ---------------------------------------------------------------------
// Diagnostics.
// ---------------------------------------------------------------------

// Print a compact pool summary to `out` — items count, extra
// allocation, bucket fill ratio, breakdown by tag. Doesn't allocate.
void ip_dump_stats(InternPool *pool, FILE *out);

// Write a human-readable representation of the type/value at `idx`
// into `buf`. Returns the number of bytes that WOULD have been written
// (snprintf-style), so callers can detect truncation. Always
// NUL-terminates if buflen > 0.
//
// Examples:
//   IP_BOOL_TYPE                       → "bool"
//   ^const u8                          → "^const u8"
//   []const u8                         → "[]const u8"
//   fn(i32, u8) -> i32                 → "fn(i32, u8) -> i32"
//   [16]u8                             → "[16]u8"
//   struct (zir=42, n_fields=2)        → "struct#42"
//   <effects: def#5, def#7>            → "<def#5, def#7>"
size_t ip_format(InternPool *pool, IpIndex idx, char *buf, size_t buflen);

#endif // ORE_DB_INTERN_POOL_H
