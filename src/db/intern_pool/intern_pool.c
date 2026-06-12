#include "intern_pool.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

// =====================================================================
// Hash function — FNV-1a 64-bit.
// =====================================================================

static inline uint64_t fnv_init(void) { return 0xcbf29ce484222325ULL; }
static inline uint64_t fnv_step(uint64_t h, const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  for (size_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}
static inline uint64_t fnv_u32(uint64_t h, uint32_t v) {
  return fnv_step(h, &v, sizeof(v));
}
static inline uint64_t fnv_u64(uint64_t h, uint64_t v) {
  return fnv_step(h, &v, sizeof(v));
}

// =====================================================================
// Per-tag payload structs (arena-stored variants only). Inline-encoded
// tags (ptr/slice/optional family) keep their payload in items_data and
// don't need a struct here.
// =====================================================================

typedef struct {
  IpIndex elem;
  uint64_t size;
} IpArrayPayload;

typedef struct {
  IpIndex ret;
  uint32_t modifiers;
  uint32_t comptime_bits;     // Phase 3 — see IpKey.fn_type.comptime_bits
  uint32_t typevalued_bits;   // Phase 3 — see IpKey.fn_type.typevalued_bits
  uint32_t n_params;
  IpIndex effect_row;  // Effects-1 — IP_EMPTY_EFFECT_ROW for pure fns
  IpIndex params[]; // n_params entries
} IpFnPayload;

// Monomorphization — IPK_INSTANCE arena payload: the declaring fn's DefId.idx
// plus its concrete arg-type vector. Mirror of IpFnPayload (scalars first,
// flexible IpIndex array last). args are POSITIONAL (call-site order) — never
// sorted, unlike effect-row labels.
typedef struct {
  uint32_t def_idx;
  uint32_t n_args;
  IpIndex  args[]; // n_args entries
} IpInstancePayload;

// Struct / enum / namespace types are INLINE-encoded (items_data = the
// nominal identity: zir_node_id for struct/enum, nsid for namespace) — they
// carry only identity; their member lists live in the recompute-friendly db
// pools (db.aggregate_field_pool / db.enum_variant_pool /
// db.namespace_field_pool). So there is no IpStructPayload / IpEnumPayload /
// IpNamespaceTypePayload arena payload anymore.

typedef struct {
  IpIndex type;
  int64_t value;
} IpIntPayload;

typedef struct {
  IpIndex type;
  double value;
} IpFloatPayload;

// Phase 4+5 — enum-variant value carrier. (enum_def, variant_idx); the
// TypedValue.type half carries the nominal enum type so no per-value type slot.
typedef struct {
  uint32_t enum_def_idx;
  uint32_t variant_idx;
} IpEnumVariantValuePayload;

typedef struct {
  uint32_t n_labels;
  IpIndex  tail;     // IP_EMPTY_EFFECT_ROW (closed) or IPK_ROW_VAR (open)
  DefId    labels[]; // sorted ascending by .idx; duplicates ALLOWED
} IpEffectRowPayload;

// Effects-1 — the `handler { … }` expression type. Inline-encoding can't fit (effect, ret)
// in a single u32, so we arena-store it like fn payloads.
typedef struct {
  IpIndex effect;
  IpIndex action;
  IpIndex ret;
} IpHandlerPayload;

// Slice 6.19 — KIND_DISTINCT's type. Two u32s (identity + backing) don't fit
// inline, so arena-store it like the handler payload.
typedef struct {
  uint32_t zir_node_id;
  IpIndex  backing;
} IpDistinctPayload;

// =====================================================================
// Storage growth.
// =====================================================================

static void items_grow(InternPool *pool, size_t need) {
  while (pool->items_cap < need) {
    size_t new_cap = pool->items_cap == 0 ? 64 : pool->items_cap * 2;
    pool->items_tag = realloc(pool->items_tag, new_cap * sizeof(IpTag));
    pool->items_data = realloc(pool->items_data, new_cap * sizeof(uint32_t));
    pool->items_cap = new_cap;
  }
}

static uint32_t append_item(InternPool *pool, IpTag tag, uint32_t data) {
  items_grow(pool, pool->items_count + 1);
  uint32_t idx = (uint32_t)pool->items_count;
  pool->items_tag[idx] = tag;
  pool->items_data[idx] = data;
  pool->items_count++;
  return idx;
}

// =====================================================================
// Bucket map.
// =====================================================================

static void buckets_init(InternPool *pool, size_t count) {
  pool->bucket_count = count;
  pool->bucket_used = 0;
  pool->buckets = calloc(count, sizeof(uint64_t));
}

// Forward decls so buckets_grow can call into reverse + eql.
static IpKey ip_key_internal(InternPool *pool, IpIndex idx);
static bool ip_key_eql(IpKey a, IpKey b);
static uint64_t hash_key(IpKey key);

static void buckets_grow(InternPool *pool) {
  uint64_t *old = pool->buckets;
  size_t old_count = pool->bucket_count;

  buckets_init(pool, old_count * 2);

  for (size_t i = 0; i < old_count; i++) {
    uint64_t e = old[i];
    if (e == 0)
      continue;

    uint32_t idx = (uint32_t)(e & 0xFFFFFFFFu) - 1;
    // Skip tombstones: re-registering a removed slot would let probes
    // find it (and ip_key_eql would compare against the error sentinel,
    // returning false, but it still wastes a probe step).
    if (pool->items_tag[idx] == IP_TAG_REMOVED)
      continue;

    IpKey k = ip_key_internal(pool, (IpIndex){idx});
    uint64_t h = hash_key(k);
    size_t mask = pool->bucket_count - 1;
    size_t b = (size_t)(h & mask);
    while (pool->buckets[b] != 0)
      b = (b + 1) & mask;
    pool->buckets[b] = ((uint64_t)(h >> 32) << 32) | (uint64_t)(idx + 1);
    pool->bucket_used++;
  }
  free(old);
}

// =====================================================================
// Hash + equality (per-IpKey-kind dispatch).
// =====================================================================

static uint64_t hash_key(IpKey key) {
  uint64_t h = fnv_init();
  h = fnv_u32(h, (uint32_t)key.kind);
  switch (key.kind) {
  case IPK_PRIMITIVE_TYPE:
    h = fnv_u32(h, (uint32_t)key.primitive_type);
    break;
  case IPK_RESERVED_VALUE:
    h = fnv_u32(h, (uint32_t)key.reserved_value);
    break;
  case IPK_PTR_TYPE:
    h = fnv_u32(h, key.ptr_type.elem.v);
    h = fnv_u32(h, key.ptr_type.is_const ? 1u : 0u);
    break;
  case IPK_MANY_PTR_TYPE:
    h = fnv_u32(h, key.many_ptr_type.elem.v);
    h = fnv_u32(h, key.many_ptr_type.is_const ? 1u : 0u);
    break;
  case IPK_SLICE_TYPE:
    h = fnv_u32(h, key.slice_type.elem.v);
    h = fnv_u32(h, key.slice_type.is_const ? 1u : 0u);
    break;
  case IPK_OPTIONAL_TYPE:
    h = fnv_u32(h, key.optional_type.elem.v);
    break;
  case IPK_ARRAY_TYPE:
    h = fnv_u32(h, key.array_type.elem.v);
    h = fnv_u64(h, key.array_type.size);
    break;
  case IPK_FN_TYPE:
    h = fnv_u32(h, key.fn_type.ret.v);
    h = fnv_u32(h, key.fn_type.modifiers);
    h = fnv_u32(h, key.fn_type.comptime_bits);
    h = fnv_u32(h, key.fn_type.typevalued_bits);
    h = fnv_u32(h, (uint32_t)key.fn_type.n_params);
    for (size_t i = 0; i < key.fn_type.n_params; i++)
      h = fnv_u32(h, key.fn_type.params[i].v);
    // Effects-1 — effect_row is part of fn-type identity. Pure-by-default
    // call sites leave the field zero-initialized, which reads as IP_NONE
    // (index 0) — not a valid effect row — so we normalize it to the
    // pre-interned empty row here (and in eq/encode) instead of forcing
    // every existing caller to touch the new field.
    {
      IpIndex er = key.fn_type.effect_row;
      if (er.v == IP_NONE.v) er = IP_EMPTY_EFFECT_ROW;
      h = fnv_u32(h, er.v);
    }
    break;
  case IPK_INSTANCE:
    // Monomorphization — (def, positional concrete args). No normalization
    // (def is a DefId, not an IpIndex with a zero sentinel); no sort (args
    // are positional). Mirror of the IPK_FN_TYPE array fold.
    h = fnv_u32(h, key.instance.def.idx);
    h = fnv_u32(h, (uint32_t)key.instance.n_args);
    for (size_t i = 0; i < key.instance.n_args; i++)
      h = fnv_u32(h, key.instance.args[i].v);
    break;
  case IPK_STRUCT_TYPE:
    // Identity = zir_node_id alone. Field set is part of the
    // *result*, not the dedup key.
    h = fnv_u32(h, key.struct_type.zir_node_id);
    break;
  case IPK_ENUM_TYPE:
    h = fnv_u32(h, key.enum_type.zir_node_id);
    break;
  case IPK_EFFECT_ROW:
#ifndef NDEBUG
    // Pre-sort assert: callers must canonicalize before ip_get so
    // <l1, l2> and <l2, l1> dedup to the same IpIndex. Duplicates are
    // ALLOWED (Koka's scoped-labels semantics: <exc, exc> ≠ <exc>) —
    // so the comparison is `<=`, not `<`.
    for (size_t i = 1; i < key.effect_row.n_labels; i++) {
      assert(key.effect_row.labels[i - 1].idx <=
                 key.effect_row.labels[i].idx &&
             "ip_get: effect_row.labels must be ascending by .idx (duplicates ok)");
    }
#endif
    h = fnv_u32(h, (uint32_t)key.effect_row.n_labels);
    for (size_t i = 0; i < key.effect_row.n_labels; i++)
      h = fnv_u32(h, key.effect_row.labels[i].idx);
    {
      // Normalize zero/IP_NONE → IP_EMPTY_EFFECT_ROW so test fixtures
      // and ad-hoc key builders that omit `tail` still dedup with the
      // reserved closed-row sentinel.
      IpIndex t = key.effect_row.tail;
      if (t.v == IP_NONE.v) t = IP_EMPTY_EFFECT_ROW;
      h = fnv_u32(h, t.v);
    }
    break;
  case IPK_ROW_VAR:
    h = fnv_u32(h, key.row_var.id);
    break;
  case IPK_TYPE_VAR:
    h = fnv_u32(h, key.type_var.id);
    break;
  case IPK_EFFECT_TYPE:
    h = fnv_u32(h, key.effect_type.zir_node_id);
    break;
  case IPK_HANDLER_TYPE:
    h = fnv_u32(h, key.handler_type.effect.v);
    h = fnv_u32(h, key.handler_type.action.v);
    h = fnv_u32(h, key.handler_type.ret.v);
    break;
  case IPK_DISTINCT_TYPE:
    h = fnv_u32(h, key.distinct_type.zir_node_id);
    h = fnv_u32(h, key.distinct_type.backing.v);
    break;
  case IPK_INT_VALUE:
    h = fnv_u32(h, key.int_value.type.v);
    h = fnv_u64(h, (uint64_t)key.int_value.value);
    break;
  case IPK_FLOAT_VALUE: {
    uint64_t bits;
    memcpy(&bits, &key.float_value.value, sizeof(bits));
    h = fnv_u32(h, key.float_value.type.v);
    h = fnv_u64(h, bits);
    break;
  }
  case IPK_NAMESPACE_VALUE:
    h = fnv_u32(h, key.namespace_value.nsid.idx);
    break;
  case IPK_ENUM_VARIANT_VALUE:
    h = fnv_u32(h, key.enum_variant_value.enum_def.idx);
    h = fnv_u32(h, key.enum_variant_value.variant_idx);
    break;
  case IPK_NAMESPACE_TYPE:
    // Nominal identity = nsid only (inline-encoded, D2.2). The member list
    // lives in db.namespace_field_pool, not the key — same as struct/enum.
    h = fnv_u32(h, key.namespace_type.nsid.idx);
    break;
  }
  return h;
}

static bool ip_key_eql(IpKey a, IpKey b) {
  if (a.kind != b.kind)
    return false;
  switch (a.kind) {
  case IPK_PRIMITIVE_TYPE:
    return a.primitive_type == b.primitive_type;
  case IPK_RESERVED_VALUE:
    return a.reserved_value == b.reserved_value;
  case IPK_PTR_TYPE:
    return a.ptr_type.elem.v == b.ptr_type.elem.v &&
           a.ptr_type.is_const == b.ptr_type.is_const;
  case IPK_MANY_PTR_TYPE:
    return a.many_ptr_type.elem.v == b.many_ptr_type.elem.v &&
           a.many_ptr_type.is_const == b.many_ptr_type.is_const;
  case IPK_SLICE_TYPE:
    return a.slice_type.elem.v == b.slice_type.elem.v &&
           a.slice_type.is_const == b.slice_type.is_const;
  case IPK_OPTIONAL_TYPE:
    return a.optional_type.elem.v == b.optional_type.elem.v;
  case IPK_ARRAY_TYPE:
    return a.array_type.elem.v == b.array_type.elem.v &&
           a.array_type.size == b.array_type.size;
  case IPK_FN_TYPE: {
    if (a.fn_type.ret.v != b.fn_type.ret.v)
      return false;
    if (a.fn_type.modifiers != b.fn_type.modifiers)
      return false;
    if (a.fn_type.comptime_bits != b.fn_type.comptime_bits)
      return false;
    if (a.fn_type.typevalued_bits != b.fn_type.typevalued_bits)
      return false;
    if (a.fn_type.n_params != b.fn_type.n_params)
      return false;
    for (size_t i = 0; i < a.fn_type.n_params; i++)
      if (a.fn_type.params[i].v != b.fn_type.params[i].v)
        return false;
    // Effects-1 — normalize matches the hash function so callers that
    // zero-init the new field still alias against an interned key whose
    // effect_row is IP_EMPTY_EFFECT_ROW.
    IpIndex aer = a.fn_type.effect_row;
    IpIndex ber = b.fn_type.effect_row;
    if (aer.v == IP_NONE.v) aer = IP_EMPTY_EFFECT_ROW;
    if (ber.v == IP_NONE.v) ber = IP_EMPTY_EFFECT_ROW;
    return aer.v == ber.v;
  }
  case IPK_INSTANCE: {
    // Monomorphization — (def, positional args). Mirror of IPK_FN_TYPE's
    // array compare; no effect-row-style normalization (no zero sentinel).
    if (a.instance.def.idx != b.instance.def.idx)
      return false;
    if (a.instance.n_args != b.instance.n_args)
      return false;
    for (size_t i = 0; i < a.instance.n_args; i++)
      if (a.instance.args[i].v != b.instance.args[i].v)
        return false;
    return true;
  }
  case IPK_STRUCT_TYPE:
    return a.struct_type.zir_node_id == b.struct_type.zir_node_id;
  case IPK_ENUM_TYPE:
    return a.enum_type.zir_node_id == b.enum_type.zir_node_id;
  case IPK_EFFECT_ROW: {
    if (a.effect_row.n_labels != b.effect_row.n_labels)
      return false;
    for (size_t i = 0; i < a.effect_row.n_labels; i++)
      if (a.effect_row.labels[i].idx != b.effect_row.labels[i].idx)
        return false;
    IpIndex at = a.effect_row.tail;
    IpIndex bt = b.effect_row.tail;
    if (at.v == IP_NONE.v) at = IP_EMPTY_EFFECT_ROW;
    if (bt.v == IP_NONE.v) bt = IP_EMPTY_EFFECT_ROW;
    return at.v == bt.v;
  }
  case IPK_ROW_VAR:
    return a.row_var.id == b.row_var.id;
  case IPK_TYPE_VAR:
    return a.type_var.id == b.type_var.id;
  case IPK_EFFECT_TYPE:
    return a.effect_type.zir_node_id == b.effect_type.zir_node_id;
  case IPK_HANDLER_TYPE:
    return a.handler_type.effect.v == b.handler_type.effect.v &&
           a.handler_type.action.v == b.handler_type.action.v &&
           a.handler_type.ret.v == b.handler_type.ret.v;
  case IPK_DISTINCT_TYPE:
    return a.distinct_type.zir_node_id == b.distinct_type.zir_node_id &&
           a.distinct_type.backing.v == b.distinct_type.backing.v;
  case IPK_INT_VALUE:
    return a.int_value.type.v == b.int_value.type.v &&
           a.int_value.value == b.int_value.value;
  case IPK_FLOAT_VALUE: {
    uint64_t ab, bb;
    memcpy(&ab, &a.float_value.value, sizeof(ab));
    memcpy(&bb, &b.float_value.value, sizeof(bb));
    return a.float_value.type.v == b.float_value.type.v && ab == bb;
  }
  case IPK_NAMESPACE_VALUE:
    return a.namespace_value.nsid.idx == b.namespace_value.nsid.idx;
  case IPK_ENUM_VARIANT_VALUE:
    return a.enum_variant_value.enum_def.idx ==
               b.enum_variant_value.enum_def.idx &&
           a.enum_variant_value.variant_idx ==
               b.enum_variant_value.variant_idx;
  case IPK_NAMESPACE_TYPE:
    return a.namespace_type.nsid.idx == b.namespace_type.nsid.idx;
  }
  return false;
}

// =====================================================================
// Reverse: ip_key_internal.
//
// HOT PATH. Inline-encoded tags read items_data directly. Arena-stored
// tags do arena_get_ptr and cast to the typed payload struct — the
// arena's cached_chunk fast path makes this one cache line in the
// common case.
// =====================================================================

static IpKey ip_key_internal(InternPool *pool, IpIndex idx) {
  if (idx.v >= pool->items_count) {
    return (IpKey){.kind = IPK_PRIMITIVE_TYPE,
                   .primitive_type = IP_INDEX_ERROR_TYPE};
  }
  IpTag tag = pool->items_tag[idx.v];
  uint32_t data = pool->items_data[idx.v];

  switch (tag) {
  case IP_TAG_PRIMITIVE_TYPE:
    return (IpKey){.kind = IPK_PRIMITIVE_TYPE,
                   .primitive_type = (enum IpReservedIndex)idx.v};
  case IP_TAG_RESERVED_VALUE:
    return (IpKey){.kind = IPK_RESERVED_VALUE,
                   .reserved_value = (enum IpReservedIndex)idx.v};

  // ---- Inline-encoded — items_data == elem.v.
  case IP_TAG_PTR_TYPE:
    return (IpKey){.kind = IPK_PTR_TYPE,
                   .ptr_type = {.elem = {.v = data}, .is_const = false}};
  case IP_TAG_PTR_CONST_TYPE:
    return (IpKey){.kind = IPK_PTR_TYPE,
                   .ptr_type = {.elem = {.v = data}, .is_const = true}};
  case IP_TAG_MANY_PTR_TYPE:
    return (IpKey){.kind = IPK_MANY_PTR_TYPE,
                   .many_ptr_type = {.elem = {.v = data}, .is_const = false}};
  case IP_TAG_MANY_PTR_CONST_TYPE:
    return (IpKey){.kind = IPK_MANY_PTR_TYPE,
                   .many_ptr_type = {.elem = {.v = data}, .is_const = true}};
  case IP_TAG_SLICE_TYPE:
    return (IpKey){.kind = IPK_SLICE_TYPE,
                   .slice_type = {.elem = {.v = data}, .is_const = false}};
  case IP_TAG_SLICE_CONST_TYPE:
    return (IpKey){.kind = IPK_SLICE_TYPE,
                   .slice_type = {.elem = {.v = data}, .is_const = true}};
  case IP_TAG_OPTIONAL_TYPE:
    return (IpKey){.kind = IPK_OPTIONAL_TYPE,
                   .optional_type = {.elem = {.v = data}}};

  // ---- Arena-stored — data is byte offset into extra_arena.
  case IP_TAG_ARRAY_TYPE: {
    const IpArrayPayload *p = arena_get_ptr(&pool->extra_arena, data);
    assert(p && "ip_key_internal: array payload out of arena range");
    return (IpKey){.kind = IPK_ARRAY_TYPE,
                   .array_type = {.elem = p->elem, .size = p->size}};
  }
  case IP_TAG_FN_TYPE: {
    const IpFnPayload *p = arena_get_ptr(&pool->extra_arena, data);
    assert(p && "ip_key_internal: fn payload out of arena range");
    IpKey k = {.kind = IPK_FN_TYPE};
    k.fn_type.ret = p->ret;
    k.fn_type.modifiers = p->modifiers;
    k.fn_type.comptime_bits = p->comptime_bits;
    k.fn_type.typevalued_bits = p->typevalued_bits;
    k.fn_type.n_params = p->n_params;
    k.fn_type.params = p->params; // stable for pool lifetime
    k.fn_type.effect_row = p->effect_row;
    return k;
  }
  case IP_TAG_INSTANCE: {
    const IpInstancePayload *p = arena_get_ptr(&pool->extra_arena, data);
    assert(p && "ip_key_internal: instance payload out of arena range");
    IpKey k = {.kind = IPK_INSTANCE};
    k.instance.def = (DefId){.idx = p->def_idx};
    k.instance.n_args = p->n_args;
    k.instance.args = p->args; // stable for pool lifetime
    return k;
  }
  // ---- Inline-encoded nominals — data == zir_node_id. Field/variant
  //      lists live in the db pools, keyed by the def (== zir_node_id).
  case IP_TAG_STRUCT_TYPE:
    return (IpKey){.kind = IPK_STRUCT_TYPE,
                   .struct_type = {.zir_node_id = data}};
  case IP_TAG_ENUM_TYPE:
    return (IpKey){.kind = IPK_ENUM_TYPE, .enum_type = {.zir_node_id = data}};
  case IP_TAG_EFFECT_ROW: {
    const IpEffectRowPayload *p = arena_get_ptr(&pool->extra_arena, data);
    assert(p && "ip_key_internal: effect_row payload out of arena range");
    IpKey k = {.kind = IPK_EFFECT_ROW};
    k.effect_row.n_labels = p->n_labels;
    k.effect_row.labels = p->labels;
    k.effect_row.tail = p->tail;
    return k;
  }
  // ---- Inline-encoded effect helpers — data is the id / zir_node_id.
  case IP_TAG_ROW_VAR:
    return (IpKey){.kind = IPK_ROW_VAR, .row_var = {.id = data}};
  case IP_TAG_TYPE_VAR:
    // Inline-encoded: data IS the id (all 32 bits; high bit reclaimed in
    // Phase 3 when TYPE_VAR.kind was deleted).
    return (IpKey){.kind = IPK_TYPE_VAR,
                   .type_var = {.id = data}};
  case IP_TAG_EFFECT_TYPE:
    return (IpKey){.kind = IPK_EFFECT_TYPE,
                   .effect_type = {.zir_node_id = data}};
  case IP_TAG_HANDLER_TYPE: {
    const IpHandlerPayload *p = arena_get_ptr(&pool->extra_arena, data);
    assert(p && "ip_key_internal: handler payload out of arena range");
    return (IpKey){
        .kind = IPK_HANDLER_TYPE,
        .handler_type = {
            .effect = p->effect, .action = p->action, .ret = p->ret}};
  }
  case IP_TAG_DISTINCT_TYPE: {
    const IpDistinctPayload *p = arena_get_ptr(&pool->extra_arena, data);
    assert(p && "ip_key_internal: distinct payload out of arena range");
    return (IpKey){.kind = IPK_DISTINCT_TYPE,
                   .distinct_type = {.zir_node_id = p->zir_node_id,
                                     .backing = p->backing}};
  }
  case IP_TAG_INT_VALUE: {
    const IpIntPayload *p = arena_get_ptr(&pool->extra_arena, data);
    assert(p && "ip_key_internal: int payload out of arena range");
    return (IpKey){.kind = IPK_INT_VALUE,
                   .int_value = {.type = p->type, .value = p->value}};
  }
  case IP_TAG_FLOAT_VALUE: {
    const IpFloatPayload *p = arena_get_ptr(&pool->extra_arena, data);
    assert(p && "ip_key_internal: float payload out of arena range");
    return (IpKey){.kind = IPK_FLOAT_VALUE,
                   .float_value = {.type = p->type, .value = p->value}};
  }

  // Phase 4+5 — inline-encoded namespace value: data IS the NamespaceId.
  case IP_TAG_NAMESPACE_VALUE:
    return (IpKey){.kind = IPK_NAMESPACE_VALUE,
                   .namespace_value = {.nsid = (NamespaceId){.idx = data}}};

  // Phase 4+5 — arena-stored enum-variant value (two u32s).
  case IP_TAG_ENUM_VARIANT_VALUE: {
    const IpEnumVariantValuePayload *p =
        arena_get_ptr(&pool->extra_arena, data);
    assert(p && "ip_key_internal: enum-variant payload out of arena range");
    return (IpKey){.kind = IPK_ENUM_VARIANT_VALUE,
                   .enum_variant_value = {
                       .enum_def = (DefId){.idx = p->enum_def_idx},
                       .variant_idx = p->variant_idx}};
  }

  // Inline-encoded nominal — data == nsid. The member list lives in
  // db.namespace_field_pool, keyed by the namespace (== nsid).
  case IP_TAG_NAMESPACE_TYPE:
    return (IpKey){.kind = IPK_NAMESPACE_TYPE,
                   .namespace_type = {.nsid = (NamespaceId){.idx = data}}};

  case IP_TAG_NONE:  // the IP_NONE sentinel — introspecting it yields poison
  case IP_TAG_REMOVED:
  case IP_TAG_COUNT: // sentinel — never stored
    return (IpKey){.kind = IPK_PRIMITIVE_TYPE,
                   .primitive_type = IP_INDEX_ERROR_TYPE};
  }
  return (IpKey){.kind = IPK_PRIMITIVE_TYPE,
                 .primitive_type = IP_INDEX_ERROR_TYPE};
}

IpKey ip_key(InternPool *pool, IpIndex idx) {
  return ip_key_internal(pool, idx);
}

// =====================================================================
// Predicates.
// =====================================================================

IpTag ip_tag(InternPool *pool, IpIndex idx) {
  if (idx.v >= pool->items_count)
    return IP_TAG_REMOVED;
  return pool->items_tag[idx.v];
}

bool ip_is_type(InternPool *pool, IpIndex idx) {
  IpTag t = ip_tag(pool, idx);
  switch (t) {
  case IP_TAG_PRIMITIVE_TYPE:
  case IP_TAG_PTR_TYPE:
  case IP_TAG_PTR_CONST_TYPE:
  case IP_TAG_MANY_PTR_TYPE:
  case IP_TAG_MANY_PTR_CONST_TYPE:
  case IP_TAG_SLICE_TYPE:
  case IP_TAG_SLICE_CONST_TYPE:
  case IP_TAG_OPTIONAL_TYPE:
  case IP_TAG_ARRAY_TYPE:
  case IP_TAG_FN_TYPE:
  case IP_TAG_STRUCT_TYPE:
  case IP_TAG_ENUM_TYPE:
  case IP_TAG_DISTINCT_TYPE:
  case IP_TAG_NAMESPACE_TYPE:
  case IP_TAG_TYPE_VAR:
  case IP_TAG_INSTANCE:
    // Namespace-as-struct-type IS a type — sema's dot-access routes
    // through the IP_TAG_NAMESPACE_TYPE branch in AST_EXPR_FIELD.
    // A distinct type IS a value type (unlike effect/handler, which are
    // excluded) — it's referenced bare in type position.
    // A type-var (anytype hole) stands in type position until ground.
    // A monomorphization instance denotes a concrete instantiated type.
    return true;
  default:
    return false;
  }
}

bool ip_is_value(InternPool *pool, IpIndex idx) {
  IpTag t = ip_tag(pool, idx);
  return t == IP_TAG_RESERVED_VALUE || t == IP_TAG_INT_VALUE ||
         t == IP_TAG_FLOAT_VALUE || t == IP_TAG_NAMESPACE_VALUE ||
         t == IP_TAG_ENUM_VARIANT_VALUE;
}

// =====================================================================
// Payload encoder — write the per-tag payload into extra_arena,
// return the byte offset.
// =====================================================================

static uint32_t encode_payload(InternPool *pool, IpKey key, IpTag tag) {
  switch (tag) {
  case IP_TAG_ARRAY_TYPE: {
    uint32_t off = (uint32_t)arena_total_used(&pool->extra_arena);
    IpArrayPayload *p = arena_alloc_raw(&pool->extra_arena, sizeof(*p));
    p->elem = key.array_type.elem;
    p->size = key.array_type.size;
    return off;
  }
  case IP_TAG_FN_TYPE: {
    size_t n = key.fn_type.n_params;
    size_t sz = sizeof(IpFnPayload) + n * sizeof(IpIndex);
    uint32_t off = (uint32_t)arena_total_used(&pool->extra_arena);
    IpFnPayload *p = arena_alloc_raw(&pool->extra_arena, sz);
    p->ret = key.fn_type.ret;
    p->modifiers = key.fn_type.modifiers;
    p->comptime_bits = key.fn_type.comptime_bits;
    p->typevalued_bits = key.fn_type.typevalued_bits;
    p->n_params = (uint32_t)n;
    // Effects-1 — normalize a zero-init/IP_NONE effect_row → IP_EMPTY_EFFECT_ROW
    // (pure-by-default), matching the same normalization in hash/eq.
    p->effect_row = (key.fn_type.effect_row.v == IP_NONE.v)
                        ? IP_EMPTY_EFFECT_ROW
                        : key.fn_type.effect_row;
    if (n > 0)
      memcpy(p->params, key.fn_type.params, n * sizeof(IpIndex));
    return off;
  }
  case IP_TAG_INSTANCE: {
    // Monomorphization — copy the borrowed positional arg array into the
    // pool's stable extra_arena (mirror of IP_TAG_FN_TYPE's params memcpy).
    size_t n = key.instance.n_args;
    size_t sz = sizeof(IpInstancePayload) + n * sizeof(IpIndex);
    uint32_t off = (uint32_t)arena_total_used(&pool->extra_arena);
    IpInstancePayload *p = arena_alloc_raw(&pool->extra_arena, sz);
    p->def_idx = key.instance.def.idx;
    p->n_args = (uint32_t)n;
    if (n > 0)
      memcpy(p->args, key.instance.args, n * sizeof(IpIndex));
    return off;
  }
  // STRUCT/ENUM are inline-encoded (see encode_items_data); they never
  // reach encode_payload.
  case IP_TAG_EFFECT_ROW: {
    size_t n = key.effect_row.n_labels;
    size_t sz = sizeof(IpEffectRowPayload) + n * sizeof(DefId);
    uint32_t off = (uint32_t)arena_total_used(&pool->extra_arena);
    IpEffectRowPayload *p = arena_alloc_raw(&pool->extra_arena, sz);
    p->n_labels = (uint32_t)n;
    p->tail = (key.effect_row.tail.v == IP_NONE.v)
                  ? IP_EMPTY_EFFECT_ROW
                  : key.effect_row.tail;
    if (n > 0)
      memcpy(p->labels, key.effect_row.labels, n * sizeof(DefId));
    return off;
  }
  case IP_TAG_HANDLER_TYPE: {
    uint32_t off = (uint32_t)arena_total_used(&pool->extra_arena);
    IpHandlerPayload *p = arena_alloc_raw(&pool->extra_arena, sizeof(*p));
    p->effect = key.handler_type.effect;
    p->action = key.handler_type.action;
    p->ret = key.handler_type.ret;
    return off;
  }
  case IP_TAG_DISTINCT_TYPE: {
    uint32_t off = (uint32_t)arena_total_used(&pool->extra_arena);
    IpDistinctPayload *p = arena_alloc_raw(&pool->extra_arena, sizeof(*p));
    p->zir_node_id = key.distinct_type.zir_node_id;
    p->backing = key.distinct_type.backing;
    return off;
  }
  // ROW_VAR and EFFECT_TYPE are inline-encoded; they never reach
  // encode_payload.
  case IP_TAG_INT_VALUE: {
    uint32_t off = (uint32_t)arena_total_used(&pool->extra_arena);
    IpIntPayload *p = arena_alloc_raw(&pool->extra_arena, sizeof(*p));
    p->type = key.int_value.type;
    p->value = key.int_value.value;
    return off;
  }
  case IP_TAG_FLOAT_VALUE: {
    uint32_t off = (uint32_t)arena_total_used(&pool->extra_arena);
    IpFloatPayload *p = arena_alloc_raw(&pool->extra_arena, sizeof(*p));
    p->type = key.float_value.type;
    p->value = key.float_value.value;
    return off;
  }
  // Phase 4+5 — enum-variant value. (NAMESPACE_VALUE is inline.)
  case IP_TAG_ENUM_VARIANT_VALUE: {
    uint32_t off = (uint32_t)arena_total_used(&pool->extra_arena);
    IpEnumVariantValuePayload *p =
        arena_alloc_raw(&pool->extra_arena, sizeof(*p));
    p->enum_def_idx = key.enum_variant_value.enum_def.idx;
    p->variant_idx  = key.enum_variant_value.variant_idx;
    return off;
  }
  // NAMESPACE_TYPE + NAMESPACE_VALUE are inline-encoded; never here.
  default:
    // Inline-encoded tags and PRIMITIVE/RESERVED/REMOVED don't pass
    // through here. Caller should have set items_data directly.
    return 0;
  }
}

// =====================================================================
// ip_get — the main entry point.
// =====================================================================

// Determine the storage tag for a key, including the const-vs-mut split
// for pointer-family tags. Returns IP_TAG_REMOVED if the kind is not
// representable (defensive — shouldn't happen).
static IpTag tag_for_key(IpKey key) {
  switch (key.kind) {
  case IPK_PRIMITIVE_TYPE:
    return IP_TAG_PRIMITIVE_TYPE;
  case IPK_RESERVED_VALUE:
    return IP_TAG_RESERVED_VALUE;
  case IPK_PTR_TYPE:
    return key.ptr_type.is_const ? IP_TAG_PTR_CONST_TYPE : IP_TAG_PTR_TYPE;
  case IPK_MANY_PTR_TYPE:
    return key.many_ptr_type.is_const ? IP_TAG_MANY_PTR_CONST_TYPE
                                      : IP_TAG_MANY_PTR_TYPE;
  case IPK_SLICE_TYPE:
    return key.slice_type.is_const ? IP_TAG_SLICE_CONST_TYPE
                                   : IP_TAG_SLICE_TYPE;
  case IPK_OPTIONAL_TYPE:
    return IP_TAG_OPTIONAL_TYPE;
  case IPK_ARRAY_TYPE:
    return IP_TAG_ARRAY_TYPE;
  case IPK_FN_TYPE:
    return IP_TAG_FN_TYPE;
  case IPK_STRUCT_TYPE:
    return IP_TAG_STRUCT_TYPE;
  case IPK_ENUM_TYPE:
    return IP_TAG_ENUM_TYPE;
  case IPK_EFFECT_ROW:
    return IP_TAG_EFFECT_ROW;
  case IPK_ROW_VAR:
    return IP_TAG_ROW_VAR;
  case IPK_TYPE_VAR:
    return IP_TAG_TYPE_VAR;
  case IPK_EFFECT_TYPE:
    return IP_TAG_EFFECT_TYPE;
  case IPK_HANDLER_TYPE:
    return IP_TAG_HANDLER_TYPE;
  case IPK_DISTINCT_TYPE:
    return IP_TAG_DISTINCT_TYPE;
  case IPK_INT_VALUE:
    return IP_TAG_INT_VALUE;
  case IPK_FLOAT_VALUE:
    return IP_TAG_FLOAT_VALUE;
  case IPK_NAMESPACE_VALUE:
    return IP_TAG_NAMESPACE_VALUE;
  case IPK_ENUM_VARIANT_VALUE:
    return IP_TAG_ENUM_VARIANT_VALUE;
  case IPK_NAMESPACE_TYPE:
    return IP_TAG_NAMESPACE_TYPE;
  case IPK_INSTANCE:
    return IP_TAG_INSTANCE;
  }
  return IP_TAG_REMOVED;
}

// Compute the items_data value for a tag — either the inline payload
// (for inline-encoded tags) or the extra_arena byte offset (for
// arena-stored tags).
static uint32_t encode_items_data(InternPool *pool, IpKey key, IpTag tag) {
  switch (tag) {
  case IP_TAG_PTR_TYPE:
  case IP_TAG_PTR_CONST_TYPE:
    return key.ptr_type.elem.v;
  case IP_TAG_MANY_PTR_TYPE:
  case IP_TAG_MANY_PTR_CONST_TYPE:
    return key.many_ptr_type.elem.v;
  case IP_TAG_SLICE_TYPE:
  case IP_TAG_SLICE_CONST_TYPE:
    return key.slice_type.elem.v;
  case IP_TAG_OPTIONAL_TYPE:
    return key.optional_type.elem.v;
  // Nominals: identity == zir_node_id, inline-encoded (fields/variants live
  // in the db pools). hash_key/ip_key_eql already key on zir alone.
  case IP_TAG_STRUCT_TYPE:
    return key.struct_type.zir_node_id;
  case IP_TAG_ENUM_TYPE:
    return key.enum_type.zir_node_id;
  case IP_TAG_NAMESPACE_TYPE:
    return key.namespace_type.nsid.idx;
  // Phase 4+5 — inline-encoded namespace VALUE (singleton type, so the
  // NamespaceId alone is the identity).
  case IP_TAG_NAMESPACE_VALUE:
    return key.namespace_value.nsid.idx;
  // Effects-1 — inline-encoded effect helpers.
  case IP_TAG_ROW_VAR:
    return key.row_var.id;
  case IP_TAG_TYPE_VAR:
    // Inline-encoded: data IS the id (full 32 bits).
    return key.type_var.id;
  case IP_TAG_EFFECT_TYPE:
    return key.effect_type.zir_node_id;
  default:
    return encode_payload(pool, key, tag);
  }
}

static IpIndex ip_get_compound(InternPool *pool, IpKey key, IpTag tag) {
  uint64_t h = hash_key(key);
  uint32_t hh = (uint32_t)(h >> 32);

  // Pre-insert grow check at 75% LF — must run BEFORE we insert so we
  // never write into a stretched table.
  if ((pool->bucket_used + 1) * 4 > pool->bucket_count * 3)
    buckets_grow(pool);

  size_t mask = pool->bucket_count - 1;
  size_t b = (size_t)(h & mask);

  while (pool->buckets[b] != 0) {
    // Hash-high filter: skip most expensive ip_key_eql calls.
    if ((uint32_t)(pool->buckets[b] >> 32) == hh) {
      uint32_t existing = (uint32_t)(pool->buckets[b] & 0xFFFFFFFFu) - 1;
      // Tombstone skip — leaving removed bucket entries in place is
      // cheap because the tag check catches them here.
      if (pool->items_tag[existing] != IP_TAG_REMOVED &&
          ip_key_eql(ip_key_internal(pool, (IpIndex){existing}), key)) {
        return (IpIndex){existing};
      }
    }
    b = (b + 1) & mask;
  }

  // Miss: encode payload, append item, register bucket.
  uint32_t data = encode_items_data(pool, key, tag);
  uint32_t new_idx = append_item(pool, tag, data);

  pool->buckets[b] = ((uint64_t)hh << 32) | (uint64_t)(new_idx + 1);
  pool->bucket_used++;
  return (IpIndex){new_idx};
}

IpIndex ip_get(InternPool *pool, IpKey key) {
  // Borrowed-payload lifetime guard. If this fires, `key` carried a
  // variable-length payload borrowed from an arena that has since been
  // reset (its generation moved) — the key was held too long, e.g.
  // across db_request_end. See IpKey.src_arena / src_gen. No-op for the
  // common case (src_arena == NULL) and compiled out under NDEBUG.
  assert((key.src_arena == NULL || key.src_arena->generation == key.src_gen) &&
         "ip_get: IpKey borrowed arrays from an arena that has since been "
         "reset — key consumed too late (held across a request boundary?)");

  // Fast paths for reserved variants — bypass the bucket map entirely.
  // The IpIndex value IS the IpReservedIndex enum value for these slots.
  switch (key.kind) {
  case IPK_PRIMITIVE_TYPE:
    return (IpIndex){(uint32_t)key.primitive_type};
  case IPK_RESERVED_VALUE:
    return (IpIndex){(uint32_t)key.reserved_value};
  default:
    break;
  }

  IpTag tag = tag_for_key(key);
  return ip_get_compound(pool, key, tag);
}

// =====================================================================
// Init / free.
// =====================================================================

static void register_bucket(InternPool *pool, IpIndex idx) {
  IpKey k = ip_key_internal(pool, idx);
  uint64_t h = hash_key(k);
  uint32_t hh = (uint32_t)(h >> 32);

  if ((pool->bucket_used + 1) * 4 > pool->bucket_count * 3)
    buckets_grow(pool);

  size_t mask = pool->bucket_count - 1;
  size_t b = (size_t)(h & mask);
  while (pool->buckets[b] != 0)
    b = (b + 1) & mask;
  pool->buckets[b] = ((uint64_t)hh << 32) | (uint64_t)(idx.v + 1);
  pool->bucket_used++;
}

// Helper: populate items_tag/items_data for every reserved slot, write
// the empty-effect-row payload, and set items_count. Caller is
// responsible for having items_grow'd to IP_RESERVED_COUNT and for
// registering the reserved compounds in the bucket map afterwards.
static void populate_reserved(InternPool *pool) {
  // ---- Index 0 — the IP_NONE sentinel. A dead slot so a zero-init IpIndex
  // reads as none (IP_TAG_NONE), never as a real type.
  pool->items_tag[IP_INDEX_NONE] = IP_TAG_NONE;
  pool->items_data[IP_INDEX_NONE] = 0;

  // ---- Primitives. items_data unused; variant recovered from idx.
#define X(lower, UPPER, SIZE, ALIGN)                                           \
  pool->items_tag[IP_INDEX_##UPPER##_TYPE] = IP_TAG_PRIMITIVE_TYPE;            \
  pool->items_data[IP_INDEX_##UPPER##_TYPE] = 0;
#include "ip_primitives.def"
#undef X

  // ---- Reserved values.
  pool->items_tag[IP_INDEX_BOOL_TRUE] = IP_TAG_RESERVED_VALUE;
  pool->items_tag[IP_INDEX_BOOL_FALSE] = IP_TAG_RESERVED_VALUE;
  pool->items_tag[IP_INDEX_VOID_VALUE] = IP_TAG_RESERVED_VALUE;
  pool->items_tag[IP_INDEX_UNDEF_VALUE] = IP_TAG_RESERVED_VALUE;
  pool->items_tag[IP_INDEX_ZERO_USIZE] = IP_TAG_RESERVED_VALUE;
  pool->items_tag[IP_INDEX_ONE_USIZE] = IP_TAG_RESERVED_VALUE;
  for (size_t i = IP_INDEX_BOOL_TRUE; i <= IP_INDEX_ONE_USIZE; i++)
    pool->items_data[i] = 0;

  // ---- Reserved compounds. Inline-encoded tags, no extra_arena alloc.
  pool->items_tag[IP_INDEX_C_STRING_TYPE] = IP_TAG_PTR_CONST_TYPE;
  pool->items_data[IP_INDEX_C_STRING_TYPE] = IP_INDEX_U8_TYPE;

  pool->items_tag[IP_INDEX_OPAQUE_PTR_TYPE] = IP_TAG_PTR_TYPE;
  pool->items_data[IP_INDEX_OPAQUE_PTR_TYPE] = IP_INDEX_VOID_TYPE;

  pool->items_tag[IP_INDEX_CONST_OPAQUE_PTR_TYPE] = IP_TAG_PTR_CONST_TYPE;
  pool->items_data[IP_INDEX_CONST_OPAQUE_PTR_TYPE] = IP_INDEX_VOID_TYPE;

  pool->items_tag[IP_INDEX_STRING_SLICE_TYPE] = IP_TAG_SLICE_CONST_TYPE;
  pool->items_data[IP_INDEX_STRING_SLICE_TYPE] = IP_INDEX_U8_TYPE;

  // ---- Empty effect row. Arena-stored (zero-length payload).
  //      Closed: tail = self-reference (sentinel — the canonical
  //      "no more labels, no row var" terminator).
  {
    uint32_t off = (uint32_t)arena_total_used(&pool->extra_arena);
    IpEffectRowPayload *p = arena_alloc_raw(&pool->extra_arena, sizeof(*p));
    p->n_labels = 0;
    p->tail = IP_EMPTY_EFFECT_ROW;
    pool->items_tag[IP_INDEX_EMPTY_EFFECT_ROW] = IP_TAG_EFFECT_ROW;
    pool->items_data[IP_INDEX_EMPTY_EFFECT_ROW] = off;
  }

  pool->items_count = IP_RESERVED_COUNT;

  // Register reserved compounds in the bucket map so user ip_get
  // calls with matching shapes return the reserved IpIndex.
  // Primitives and reserved values are NOT registered — they take
  // the fast path in ip_get.
  register_bucket(pool, (IpIndex){IP_INDEX_C_STRING_TYPE});
  register_bucket(pool, (IpIndex){IP_INDEX_OPAQUE_PTR_TYPE});
  register_bucket(pool, (IpIndex){IP_INDEX_CONST_OPAQUE_PTR_TYPE});
  register_bucket(pool, (IpIndex){IP_INDEX_STRING_SLICE_TYPE});
  register_bucket(pool, (IpIndex){IP_INDEX_EMPTY_EFFECT_ROW});
}

void ip_init_with(InternPool *pool, size_t initial_buckets,
                  size_t extra_chunk_size) {
  assert(initial_buckets >= 16 &&
         (initial_buckets & (initial_buckets - 1)) == 0 &&
         "ip_init_with: initial_buckets must be power of two >= 16");
  assert(extra_chunk_size >= 64 &&
         "ip_init_with: extra_chunk_size must be >= 64");

  memset(pool, 0, sizeof(*pool));
  items_grow(pool, IP_RESERVED_COUNT);

  // Initialize extra_arena with the requested chunk size so the first
  // allocation gets a properly-sized chunk instead of the arena.c
  // default 4096.
  arena_init(&pool->extra_arena, extra_chunk_size);

  buckets_init(pool, initial_buckets);
  populate_reserved(pool);
}

void ip_init(InternPool *pool) {
  ip_init_with(pool, /*initial_buckets=*/256, /*extra_chunk_size=*/4096);
}

void ip_clear(InternPool *pool) {
  // Reuse existing allocations: zero bucket table, reset extra_arena
  // (drops chunks past the first, resets first chunk's used to 0),
  // re-run reserved population.
  memset(pool->buckets, 0, pool->bucket_count * sizeof(uint64_t));
  pool->bucket_used = 0;

  arena_reset(&pool->extra_arena);
  pool->items_count = 0;
  pool->next_row_var_id = 0;  // Effects-1 counter
  pool->next_type_var_id = 0; // anytype-hole counter

  populate_reserved(pool);
}

IpIndex ip_fresh_row_var(InternPool *pool) {
  uint32_t id = ++pool->next_row_var_id; // 0 reserved (== uninitialized)
  IpKey k = {.kind = IPK_ROW_VAR, .row_var = {.id = id}};
  return ip_get(pool, k);
}

IpIndex ip_fresh_type_var(InternPool *pool) {
  uint32_t id = ++pool->next_type_var_id; // 0 reserved (== uninitialized)
  IpKey k = {.kind = IPK_TYPE_VAR, .type_var = {.id = id}};
  return ip_get(pool, k);
}

void ip_free(InternPool *pool) {
  free(pool->items_tag);
  free(pool->items_data);
  free(pool->buckets);
  arena_free(&pool->extra_arena);
  memset(pool, 0, sizeof(*pool));
}

// =====================================================================
// (No WipContainer / two-phase construction.)
//
// ip_wip_struct/_finish/_cancel were removed in D2.1b; ip_wip_fn_* in the
// D2 audit. ALL nominal AND structural types now intern via a single plain
// ip_get up front:
//   - struct/enum/namespace: identity = zir_node_id / nsid (inline-encoded),
//     deduped + stable on the first ip_get; self-reference resolves through
//     the type cell type_of_def publishes before its field loop.
//   - fn types: build_fn_type resolves ret + params FIRST (recursing through
//     the already-published self type cell for `fn(self:^Self) Self`), THEN
//     does one ip_get(IPK_FN_TYPE, …) — so the structural identity is known
//     before interning and no IpIndex needs pre-reserving.
// Because nothing reserves an un-encoded entry anymore, items_data is always
// a real payload/elem value (no WIP sentinel), so ip_key_internal needs no
// in-flight guard.
// =====================================================================

// =====================================================================
// Removal & compaction.
// =====================================================================

void ip_remove(InternPool *pool, IpIndex idx) {
  if (idx.v >= pool->items_count)
    return;
  pool->items_tag[idx.v] = IP_TAG_REMOVED;
}

bool ip_compact(InternPool *pool) {
  (void)pool;
  // Deferred. Trigger criterion (when implemented):
  //   removed_count > items_count / 4
  // Implementation outline:
  //   1. Walk items_tag, collect non-removed indices.
  //   2. Build a remapping table old_idx → new_idx.
  //   3. Allocate fresh items_tag, items_data, extra_arena, buckets.
  //   4. For each surviving entry, call ip_key_internal(old) to get
  //      its key, then ip_get_compound(new_pool, key, tag) to insert
  //      into the new storage.
  //   5. Swap into pool.
  //   6. Caller is responsible for remapping any external IpIndex
  //      references via the remapping table.
  return false;
}

// =====================================================================
// Diagnostics.
// =====================================================================

void ip_dump_stats(InternPool *pool, FILE *out) {
  if (!pool || !out)
    return;

  size_t per_tag[IP_TAG_COUNT] = {0};
  size_t removed = 0;
  for (size_t i = 0; i < pool->items_count; i++) {
    if (pool->items_tag[i] == IP_TAG_REMOVED) {
      removed++;
      continue;
    }
    per_tag[pool->items_tag[i]]++;
  }

  fprintf(
      out,
      "intern_pool: items=%zu (removed=%zu) extra_used=%zu buckets=%zu/%zu\n",
      pool->items_count, removed, arena_total_used(&pool->extra_arena),
      pool->bucket_used, pool->bucket_count);
  fprintf(out, "  primitive_type      %zu\n", per_tag[IP_TAG_PRIMITIVE_TYPE]);
  fprintf(out, "  reserved_value      %zu\n", per_tag[IP_TAG_RESERVED_VALUE]);
  fprintf(out, "  ptr_type            %zu\n", per_tag[IP_TAG_PTR_TYPE]);
  fprintf(out, "  ptr_const_type      %zu\n", per_tag[IP_TAG_PTR_CONST_TYPE]);
  fprintf(out, "  many_ptr_type       %zu\n", per_tag[IP_TAG_MANY_PTR_TYPE]);
  fprintf(out, "  many_ptr_const_type %zu\n",
          per_tag[IP_TAG_MANY_PTR_CONST_TYPE]);
  fprintf(out, "  slice_type          %zu\n", per_tag[IP_TAG_SLICE_TYPE]);
  fprintf(out, "  slice_const_type    %zu\n", per_tag[IP_TAG_SLICE_CONST_TYPE]);
  fprintf(out, "  optional_type       %zu\n", per_tag[IP_TAG_OPTIONAL_TYPE]);
  fprintf(out, "  array_type          %zu\n", per_tag[IP_TAG_ARRAY_TYPE]);
  fprintf(out, "  fn_type             %zu\n", per_tag[IP_TAG_FN_TYPE]);
  fprintf(out, "  struct_type         %zu\n", per_tag[IP_TAG_STRUCT_TYPE]);
  fprintf(out, "  enum_type           %zu\n", per_tag[IP_TAG_ENUM_TYPE]);
  fprintf(out, "  namespace_type      %zu\n", per_tag[IP_TAG_NAMESPACE_TYPE]);
  fprintf(out, "  effect_row          %zu\n", per_tag[IP_TAG_EFFECT_ROW]);
  fprintf(out, "  row_var             %zu\n", per_tag[IP_TAG_ROW_VAR]);
  fprintf(out, "  effect_type         %zu\n", per_tag[IP_TAG_EFFECT_TYPE]);
  fprintf(out, "  handler_type        %zu\n", per_tag[IP_TAG_HANDLER_TYPE]);
  fprintf(out, "  distinct_type       %zu\n", per_tag[IP_TAG_DISTINCT_TYPE]);
  fprintf(out, "  int_value           %zu\n", per_tag[IP_TAG_INT_VALUE]);
  fprintf(out, "  float_value         %zu\n", per_tag[IP_TAG_FLOAT_VALUE]);
  fprintf(out, "  namespace_value     %zu\n",
          per_tag[IP_TAG_NAMESPACE_VALUE]);
  fprintf(out, "  enum_variant_value  %zu\n",
          per_tag[IP_TAG_ENUM_VARIANT_VALUE]);
  fprintf(out, "  type_var            %zu\n", per_tag[IP_TAG_TYPE_VAR]);
  fprintf(out, "  instance            %zu\n", per_tag[IP_TAG_INSTANCE]);
}

// =====================================================================
// Pretty-print — ip_format.
//
// Recursive walk of an IpKey, writing a human-readable string. Depth
// is capped to handle (illegal but possible) self-referential cycles.
// Buffer behavior is snprintf-style: write up to buflen-1 bytes plus
// NUL; return the count of bytes that WOULD have been written.
// =====================================================================

// Primitive name table, generated from ip_primitives.def's lower column.
// The table is 0-based but the primitive range starts at IP_INDEX_BOOL_TYPE
// (= 1; index 0 is the IP_NONE dead sentinel), so lookups subtract the base.
static const char *const ip_primitive_names[] = {
#define X(lower, UPPER, SIZE, ALIGN) #lower,
#include "ip_primitives.def"
#undef X
};

typedef struct {
  char *buf;
  size_t cap;
  size_t pos; // bytes appended (excluding NUL) — may exceed cap
} FmtBuf;

static void fb_putc(FmtBuf *fb, char c) {
  if (fb->cap > 0 && fb->pos + 1 < fb->cap)
    fb->buf[fb->pos] = c;
  fb->pos++;
}

static void fb_puts(FmtBuf *fb, const char *s) {
  while (*s)
    fb_putc(fb, *s++);
}

static void fb_putu(FmtBuf *fb, uint64_t v) {
  char tmp[24];
  int n = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)v);
  if (n > 0)
    for (int i = 0; i < n; i++)
      fb_putc(fb, tmp[i]);
}

static void fb_puti(FmtBuf *fb, int64_t v) {
  char tmp[24];
  int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
  if (n > 0)
    for (int i = 0; i < n; i++)
      fb_putc(fb, tmp[i]);
}

#define IP_FORMAT_MAX_DEPTH 16

static void format_recursive(FmtBuf *fb, InternPool *pool, IpIndex idx,
                             int depth) {
  if (depth > IP_FORMAT_MAX_DEPTH) {
    fb_puts(fb, "...");
    return;
  }

  if (idx.v >= pool->items_count) {
    fb_puts(fb, "<oob>");
    return;
  }
  if (pool->items_tag[idx.v] == IP_TAG_REMOVED) {
    fb_puts(fb, "<removed>");
    return;
  }

  IpKey k = ip_key_internal(pool, idx);
  switch (k.kind) {
  case IPK_PRIMITIVE_TYPE: {
    uint32_t v = (uint32_t)k.primitive_type;
    if (v >= (uint32_t)IP_INDEX_BOOL_TYPE && v < (uint32_t)IP_INDEX_BOOL_TRUE) {
      fb_puts(fb, ip_primitive_names[v - (uint32_t)IP_INDEX_BOOL_TYPE]);
    } else {
      fb_puts(fb, "<bad-primitive>");
    }
    break;
  }
  case IPK_RESERVED_VALUE:
    switch (k.reserved_value) {
    case IP_INDEX_BOOL_TRUE:
      fb_puts(fb, "true");
      break;
    case IP_INDEX_BOOL_FALSE:
      fb_puts(fb, "false");
      break;
    case IP_INDEX_VOID_VALUE:
      fb_puts(fb, "void");
      break;
    case IP_INDEX_UNDEF_VALUE:
      fb_puts(fb, "undef");
      break;
    case IP_INDEX_ZERO_USIZE:
      fb_puts(fb, "0_usize");
      break;
    case IP_INDEX_ONE_USIZE:
      fb_puts(fb, "1_usize");
      break;
    default:
      fb_puts(fb, "<bad-reserved>");
    }
    break;
  case IPK_PTR_TYPE:
    fb_puts(fb, k.ptr_type.is_const ? "^const " : "^");
    format_recursive(fb, pool, k.ptr_type.elem, depth + 1);
    break;
  case IPK_MANY_PTR_TYPE:
    fb_puts(fb, k.many_ptr_type.is_const ? "[^]const " : "[^]");
    format_recursive(fb, pool, k.many_ptr_type.elem, depth + 1);
    break;
  case IPK_SLICE_TYPE:
    fb_puts(fb, k.slice_type.is_const ? "[]const " : "[]");
    format_recursive(fb, pool, k.slice_type.elem, depth + 1);
    break;
  case IPK_OPTIONAL_TYPE:
    fb_putc(fb, '?');
    format_recursive(fb, pool, k.optional_type.elem, depth + 1);
    break;
  case IPK_ARRAY_TYPE:
    fb_putc(fb, '[');
    fb_putu(fb, k.array_type.size);
    fb_putc(fb, ']');
    format_recursive(fb, pool, k.array_type.elem, depth + 1);
    break;
  case IPK_FN_TYPE:
    fb_puts(fb, "fn(");
    for (size_t i = 0; i < k.fn_type.n_params; i++) {
      if (i > 0)
        fb_puts(fb, ", ");
      format_recursive(fb, pool, k.fn_type.params[i], depth + 1);
    }
    // Effects-1 — emit the effect row only when non-empty so existing
    // diag/test output stays byte-identical for pure fns.
    if (k.fn_type.effect_row.v != IP_EMPTY_EFFECT_ROW.v &&
        k.fn_type.effect_row.v != 0 &&
        k.fn_type.effect_row.v != IP_NONE.v) {
      fb_puts(fb, ") ");
      format_recursive(fb, pool, k.fn_type.effect_row, depth + 1);
      fb_puts(fb, " -> ");
    } else {
      fb_puts(fb, ") -> ");
    }
    format_recursive(fb, pool, k.fn_type.ret, depth + 1);
    break;
  case IPK_STRUCT_TYPE:
    fb_puts(fb, "struct#");
    fb_putu(fb, k.struct_type.zir_node_id);
    break;
  case IPK_ENUM_TYPE:
    fb_puts(fb, "enum#");
    fb_putu(fb, k.enum_type.zir_node_id);
    break;
  case IPK_EFFECT_ROW:
    fb_putc(fb, '<');
    for (size_t i = 0; i < k.effect_row.n_labels; i++) {
      if (i > 0)
        fb_puts(fb, ", ");
      fb_puts(fb, "def#");
      fb_putu(fb, k.effect_row.labels[i].idx);
    }
    // Render the row tail only when open (a row var). The closed
    // sentinel — IP_EMPTY_EFFECT_ROW — is the silent terminator and
    // produces e.g. `<>` or `<exn>` exactly as before.
    if (k.effect_row.tail.v != IP_EMPTY_EFFECT_ROW.v) {
      if (k.effect_row.n_labels > 0) fb_puts(fb, " | ");
      else fb_puts(fb, "..");
      format_recursive(fb, pool, k.effect_row.tail, depth + 1);
    }
    fb_putc(fb, '>');
    break;
  case IPK_ROW_VAR:
    fb_puts(fb, "rv#");
    fb_putu(fb, k.row_var.id);
    break;
  case IPK_TYPE_VAR:
    fb_puts(fb, "tv#");
    fb_putu(fb, k.type_var.id);
    break;
  case IPK_INSTANCE:
    fb_puts(fb, "inst<def#");
    fb_putu(fb, k.instance.def.idx);
    fb_puts(fb, ">(");
    for (size_t i = 0; i < k.instance.n_args; i++) {
      if (i > 0)
        fb_puts(fb, ", ");
      format_recursive(fb, pool, k.instance.args[i], depth + 1);
    }
    fb_putc(fb, ')');
    break;
  case IPK_EFFECT_TYPE:
    fb_puts(fb, "effect#");
    fb_putu(fb, k.effect_type.zir_node_id);
    break;
  case IPK_HANDLER_TYPE:
    fb_puts(fb, "handler<");
    format_recursive(fb, pool, k.handler_type.effect, depth + 1);
    fb_puts(fb, ">(");
    format_recursive(fb, pool, k.handler_type.action, depth + 1);
    fb_puts(fb, ") -> ");
    format_recursive(fb, pool, k.handler_type.ret, depth + 1);
    break;
  case IPK_DISTINCT_TYPE:
    fb_puts(fb, "distinct ");
    format_recursive(fb, pool, k.distinct_type.backing, depth + 1);
    fb_puts(fb, "#");
    fb_putu(fb, k.distinct_type.zir_node_id);
    break;
  case IPK_INT_VALUE:
    fb_puti(fb, k.int_value.value);
    fb_puts(fb, ": ");
    format_recursive(fb, pool, k.int_value.type, depth + 1);
    break;
  case IPK_FLOAT_VALUE: {
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%g", k.float_value.value);
    if (n > 0)
      for (int i = 0; i < n; i++)
        fb_putc(fb, tmp[i]);
    fb_puts(fb, ": ");
    format_recursive(fb, pool, k.float_value.type, depth + 1);
    break;
  }
  case IPK_NAMESPACE_VALUE:
    fb_puts(fb, "namespace_value#");
    fb_putu(fb, k.namespace_value.nsid.idx);
    break;
  case IPK_ENUM_VARIANT_VALUE:
    fb_puts(fb, "variant<enum#");
    fb_putu(fb, k.enum_variant_value.enum_def.idx);
    fb_puts(fb, ">[");
    fb_putu(fb, k.enum_variant_value.variant_idx);
    fb_putc(fb, ']');
    break;
  case IPK_NAMESPACE_TYPE:
    // Inline nominal — identity is the nsid; the member list lives in the db,
    // which the intern pool doesn't back-reference. Print `namespace#<nsid>`.
    fb_puts(fb, "namespace#");
    fb_putu(fb, k.namespace_type.nsid.idx);
    break;
  }
}

size_t ip_format(InternPool *pool, IpIndex idx, char *buf, size_t buflen) {
  FmtBuf fb = {.buf = buf, .cap = buflen, .pos = 0};
  if (buflen > 0)
    buf[0] = '\0';

  format_recursive(&fb, pool, idx, 0);

  if (buflen > 0) {
    // Always NUL-terminate within the caller's buffer.
    size_t terminator = fb.pos < buflen ? fb.pos : buflen - 1;
    buf[terminator] = '\0';
  }
  return fb.pos;
}
