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

// The "none" sentinel IS intern index 0 (IP_INDEX_NONE), seeded as a dead
// IP_TAG_NONE slot. This is the universal 0==none==NULL==zero-init alignment:
// a zero-initialized IpIndex reads as none (not as a real type), and a real
// type's index (≥1) stored as a void* hashmap value is never NULL. (Was
// UINT32_MAX, which left index 0 == bool and bred the bool/index-0 collisions.)
#define IP_NONE ((IpIndex){0})

static inline bool ip_index_is_valid(IpIndex i) { return i.v != 0; }
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
    // Index 0 is the NONE sentinel (IP_NONE == {0}) — a dead IP_TAG_NONE slot,
    // never a real type. Keeping a real type off index 0 is what makes
    // zero-init == none and void*-hashmap storage NULL-safe. Everything below
    // shifts +1 (bool becomes index 1).
    IP_INDEX_NONE = 0,

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
    IP_TAG_EFFECT_ROW,            // <l1, l2, … | tail> — sorted DefId labels (duplicates kept) + tail
    IP_TAG_ROW_VAR,               // row variable — items_data == id (inline)
    IP_TAG_TYPE_VAR,              // type variable / anytype hole — items_data == id (inline)
    IP_TAG_EFFECT_TYPE,           // KIND_EFFECT decl's type — items_data == zir_node_id (inline)
    IP_TAG_HANDLER_TYPE,          // handler value's type — arena payload (effect, ret)
    IP_TAG_DISTINCT_TYPE,         // KIND_DISTINCT's type — arena payload (zir_node_id, backing)

    IP_TAG_INT_VALUE,
    IP_TAG_FLOAT_VALUE,

    // Phase 4+5 — TypedValue.value carriers. Replace const_eval.c's
    // CONST_NAMESPACE / CONST_ENUM_VARIANT discriminator entries.
    //   IP_TAG_NAMESPACE_VALUE: inline-encoded, items_data == NamespaceId.idx.
    //     A namespace value (e.g. result of `@import("x")`); its TypedValue
    //     type is IP_TYPE_TYPE (a namespace IS a type).
    //   IP_TAG_ENUM_VARIANT_VALUE: arena-stored, items_data == byte offset
    //     into extra_arena. Carries (enum_def, variant_idx); its TypedValue
    //     type is the nominal enum type derived from enum_def.
    IP_TAG_NAMESPACE_VALUE,
    IP_TAG_ENUM_VARIANT_VALUE,
    //   IP_TAG_FN_VALUE: inline-encoded, items_data == DefId.idx. A function
    //     reference as a comptime value (Zig's `func` value); its TypedValue
    //     type is the fn type. Carries the callee's top-level DefId so a call
    //     site recovers it uniformly (bare or qualified), keying monomorphization.
    IP_TAG_FN_VALUE,

    // File-as-namespace struct type. Identity = (nsid, field set).
    // Stores (StrId field_name, DefId field_def) pairs. Field TYPES are
    // resolved lazily via db_query_type_of_def(field_def) on access —
    // matches Zig's Namespace.owner_type (sema looks up the Nav, then
    // analyzes the Nav's type on demand). Sema's dot-access on this
    // tag falls back to the same struct-field-lookup machinery as
    // IP_TAG_STRUCT_TYPE.
    IP_TAG_NAMESPACE_TYPE,

    // Monomorphization — a concrete instance of a generic fn:
    // (DefId def, concrete arg types). Arena payload (def_idx, n_args, args[]),
    // mirroring IP_TAG_FN_TYPE's borrowed-array storage.
    IP_TAG_INSTANCE,

    // The IP_NONE sentinel, seeded at index 0. Not a type and not a value —
    // ip_is_type/ip_is_value both return false; ip_key decodes it to the
    // error-primitive (none introspected = poison). Lets a zero-init
    // IpIndex be read safely as "none".
    IP_TAG_NONE,

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
    // Phase Effects-1 — additive variants for effect typing.
    // IPK_ROW_VAR: a fresh row variable introduced by a polymorphic
    //   fn signature (one unique id per signature). Acts as "the
    //   rest of the row" — interns as a distinct IpIndex per id.
    // IPK_EFFECT_TYPE: the type of an `effect Foo { … }` decl —
    //   i.e. what type_of_def returns for a KIND_EFFECT def.
    // IPK_HANDLER_TYPE: the type of a `handler { … }` expression (a
    //   first-class VALUE — there is no KIND_HANDLER decl). References the
    //   discharged effect + the return type the handler produces.
    // IPK_DISTINCT_TYPE: the type of a `MyT :: distinct u8` decl (KIND_DISTINCT).
    //   Nominal identity = the declaring def's zir_node_id; also carries the
    //   backing type the newtype wraps.
    IPK_ROW_VAR,
    // IPK_TYPE_VAR: a fresh type variable / `anytype` hole introduced by a
    //   polymorphic fn signature (one unique id per anytype param). Interns
    //   as a distinct IpIndex per id; resolved/bound via SemaCtx.type_subst
    //   (mirror of IPK_ROW_VAR + row_subst).
    IPK_TYPE_VAR,
    IPK_EFFECT_TYPE,
    IPK_HANDLER_TYPE,
    IPK_DISTINCT_TYPE,

    IPK_INT_VALUE,
    IPK_FLOAT_VALUE,

    // Phase 4+5 — TypedValue.value kinds for comptime-folded namespace /
    // enum-variant payloads. Replace const_eval's CONST_NAMESPACE /
    // CONST_ENUM_VARIANT slots; encoded inline (namespace) and arena-stored
    // (enum variant, two u32s).
    IPK_NAMESPACE_VALUE,
    IPK_ENUM_VARIANT_VALUE,
    IPK_FN_VALUE, // function reference as a comptime value (carries its DefId)

    IPK_NAMESPACE_TYPE, // file-as-namespace struct type — payload is (nsid, field names, field DefIds)

    // Monomorphization — a generic fn specialized to concrete arg types.
    // Identity = (declaring DefId, the borrowed concrete-arg-type array). The
    // interned IpIndex.v IS the QUERY_INFER_INSTANCE routing key. Mirrors
    // IPK_FN_TYPE's scalar + borrowed-IpIndex[] storage.
    IPK_INSTANCE,
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
            // Per-param flags. bit i set ⇒ argument i is comptime-evaluated
            // by eval_expr at the call site (Zig-style). 32-param ceiling.
            //   comptime_bits[i] = 1, typevalued_bits[i] = 1 → `t: type`
            //   comptime_bits[i] = 1, typevalued_bits[i] = 0 → `anytype`
            //     (or future `comptime n: u32`)
            //   comptime_bits[i] = 0, typevalued_bits[i] = 0 → regular runtime param
            // Replaces per-hole TYPE_VAR.kind (deleted).
            uint32_t       comptime_bits;
            uint32_t       typevalued_bits;
            const IpIndex *params;     // borrowed; valid for pool lifetime
            size_t         n_params;
            // Effects-1 — the row of effects this fn signature carries.
            // IP_EMPTY_EFFECT_ROW for pure callees; pure-by-default for every
            // existing call site that omits the field.
            IpIndex        effect_row;
        } fn_type;

        // Nominal types: identity is the declaring def (zir_node_id). Field /
        // variant lists are NOT here — they live in the recompute-friendly db
        // pools (db.struct_field_pool / db.enum_variant_pool), keyed by the
        // def (== zir_node_id). The pool stores only this stable identity,
        // inline-encoded.
        struct { uint32_t zir_node_id; } struct_type;
        struct { uint32_t zir_node_id; } enum_type;

        // Scoped-labels effect row (Leijen 2005, 2017). The label list is
        // sorted by DefId.idx but DUPLICATES ARE PRESERVED — never deduped
        // — so `<exc, exc>` ≠ `<exc>` (inner-handler-wins). The tail is
        // either IP_EMPTY_EFFECT_ROW (closed) or a row-var IpIndex (open).
        struct {
            const DefId   *labels;    // sorted by .idx ascending; duplicates allowed; borrowed; pool-lifetime
            size_t         n_labels;
            IpIndex        tail;      // IP_EMPTY_EFFECT_ROW (closed) or IPK_ROW_VAR (open)
        } effect_row;

        // A row variable — just a fresh id. One unique row var per polymorphic
        // fn-signature instance; bound in the SemaCtx substitution table during
        // inference.
        struct { uint32_t id; } row_var;

        // A type variable / generic-parameter hole — just a fresh id.
        // The "is this arg evaluated as a type or as a value" decision lives
        // on fn_type.comptime_bits / typevalued_bits, not on the hole.
        struct {
          uint32_t id;
        } type_var;

        // Monomorphization instance — a generic fn def specialized to a
        // concrete arg-type vector. `args` is borrowed (pool-lifetime after
        // intern); the scalar `def` sits alongside it, exactly as fn_type's
        // scalars sit alongside `params`. Positional (NOT sorted).
        struct {
            DefId          def;
            const IpIndex *args;     // concrete per-param types; borrowed; pool-lifetime
            size_t         n_args;
        } instance;

        // KIND_EFFECT's type_of_def — nominal identity = declaring def.
        // Op signatures live alongside the def, not in the key.
        struct { uint32_t zir_node_id; } effect_type;

        // The type of a `handler { … }` expression — Koka's (a, b, l): the
        // discharged effect `l` (effect), the ACTION result type `a` (action,
        // = `return(x: T)`'s T; IP_NONE when unannotated → pass-through), and
        // the ANSWER type `b` (ret, = the return-clause body / op-clause bodies).
        struct {
            IpIndex effect;
            IpIndex action;
            IpIndex ret;
        } handler_type;

        // KIND_DISTINCT's type — nominal identity = the declaring def's
        // zir_node_id; `backing` is the wrapped type. Arena-stored.
        struct {
            uint32_t zir_node_id;
            IpIndex  backing;
        } distinct_type;

        struct { IpIndex type; int64_t value; } int_value;
        struct { IpIndex type; double  value; } float_value;

        // Phase 4+5 — namespace value (`@import` result). Inline-encoded:
        // identity is the NamespaceId alone (a namespace's IpIndex.type is
        // always IP_TYPE_TYPE — no per-value type slot needed).
        struct { NamespaceId nsid; } namespace_value;

        // Phase 4+5 — enum-variant value (qualified `Color.Red` or bare
        // `.Red` resolved against an enum hint). Arena-stored; identity is
        // (enum_def, variant_idx). The owning TypedValue's `.type` half
        // carries the nominal enum type, so no per-value type slot.
        struct { DefId enum_def; uint32_t variant_idx; } enum_variant_value;

        // Function reference as a comptime value (Zig's `func` value). Inline-
        // encoded: identity is the DefId alone (its IpIndex.type is the fn type,
        // not stored per-value). Lets a call site recover the callee's top-level
        // DefId from the evaluated callee, uniform for bare + qualified callees.
        struct { DefId def; } fn_value;

        // File-as-namespace struct type. Nominal identity = nsid (inline-
        // encoded). The exported (name → DefId) member list lives in
        // db.namespace_field_pool, keyed by the namespace; member TYPES are
        // resolved lazily via db_query_type_of_def(member.def). Matches Zig's
        // Namespace.owner_type.
        struct { NamespaceId nsid; } namespace_type;
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

    // Effects-1 — monotonic counter for IPK_ROW_VAR ids. Each fresh row
    // variable introduced during type construction bumps this counter,
    // so distinct row vars never alias. Reset by ip_clear; persistent
    // across normal request boundaries (row var identity must outlive
    // the request_arena).
    uint32_t  next_row_var_id;

    // Monotonic counter for IPK_TYPE_VAR ids (mirror of next_row_var_id).
    // Each fresh anytype hole bumps it; reset by ip_clear.
    uint32_t  next_type_var_id;
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

// Effects-1 — intern a fresh row variable. Bumps next_row_var_id then
// interns IPK_ROW_VAR. Each call yields a distinct IpIndex; callers
// use these in IPK_EFFECT_ROW.tail to mark the row as open/polymorphic.
IpIndex ip_fresh_row_var(InternPool *pool);

// Intern a fresh type variable hole (mirror of ip_fresh_row_var). Bumps
// next_type_var_id then interns IPK_TYPE_VAR. Each call yields a distinct
// IpIndex; bound via SemaCtx.type_subst during inference. The "type vs
// value" interpretation lives on the owning fn_type's per-param bit-words.
IpIndex ip_fresh_type_var(InternPool *pool);


// ---------------------------------------------------------------------
// No two-phase (wip) construction.
// ---------------------------------------------------------------------
//
// Self-referential nominals (`struct Node { next: ^Node }`) and self-
// referential STRUCTURAL fn types (`fn dispatch(self: ^Self) -> Self`) do
// NOT need a reserved-then-patched IpIndex. Nominals are inline-encoded
// (identity = zir_node_id / nsid), so a plain ip_get yields a stable deduped
// index up front; the self-reference resolves through the type cell
// type_of_def publishes before its field loop. Fn types resolve ret + params
// first (recursing through that published cell) and then do a single
// structural ip_get. The ip_wip_* API (struct: removed D2.1b; fn: removed in
// the D2 audit) is therefore gone — every type interns via one ip_get.
//
// Identity (dedup key): structs/enums/namespaces — zir_node_id / nsid (NOT
// the field/variant set; two same-shaped decls at different sites are
// distinct types). fn types — structural (ret + modifiers + params).


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
