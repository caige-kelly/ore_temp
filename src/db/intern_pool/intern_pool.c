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
  uint32_t n_params;
  IpIndex params[]; // n_params entries
} IpFnPayload;

typedef struct {
  uint32_t zir_node_id;
  uint32_t n_fields;
  uint32_t tail[]; // [0 .. n_fields-1]: field names (StrId.v)
                   // [n_fields .. 2*n_fields-1]: field types (IpIndex.v)
} IpStructPayload;

// Enums use sibling arena allocations for names[] and values[] to
// avoid the u32-vs-i64 alignment mess in a single packed tail. names
// and values offsets live in the header; addresses are recovered via
// arena_get_ptr.
typedef struct {
  uint32_t zir_node_id;
  uint32_t n_variants;
  uint32_t names_offset;  // byte offset in extra_arena
  uint32_t values_offset; // byte offset in extra_arena
} IpEnumPayload;

typedef struct {
  IpIndex type;
  int64_t value;
} IpIntPayload;

typedef struct {
  IpIndex type;
  double value;
} IpFloatPayload;

typedef struct {
  uint32_t n_effects;
  DefId effects[]; // sorted ascending by .idx
} IpEffectRowPayload;

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
    h = fnv_u32(h, (uint32_t)key.fn_type.n_params);
    for (size_t i = 0; i < key.fn_type.n_params; i++)
      h = fnv_u32(h, key.fn_type.params[i].v);
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
    // {E1, E2} and {E2, E1} dedup to the same IpIndex.
    for (size_t i = 1; i < key.effect_row.n_effects; i++) {
      assert(key.effect_row.effects[i - 1].idx <
                 key.effect_row.effects[i].idx &&
             "ip_get: effect_row.effects must be strictly ascending by .idx");
    }
#endif
    h = fnv_u32(h, (uint32_t)key.effect_row.n_effects);
    for (size_t i = 0; i < key.effect_row.n_effects; i++)
      h = fnv_u32(h, key.effect_row.effects[i].idx);
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
  case IPK_FN_TYPE:
    if (a.fn_type.ret.v != b.fn_type.ret.v)
      return false;
    if (a.fn_type.modifiers != b.fn_type.modifiers)
      return false;
    if (a.fn_type.n_params != b.fn_type.n_params)
      return false;
    for (size_t i = 0; i < a.fn_type.n_params; i++)
      if (a.fn_type.params[i].v != b.fn_type.params[i].v)
        return false;
    return true;
  case IPK_STRUCT_TYPE:
    return a.struct_type.zir_node_id == b.struct_type.zir_node_id;
  case IPK_ENUM_TYPE:
    return a.enum_type.zir_node_id == b.enum_type.zir_node_id;
  case IPK_EFFECT_ROW:
    if (a.effect_row.n_effects != b.effect_row.n_effects)
      return false;
    for (size_t i = 0; i < a.effect_row.n_effects; i++)
      if (a.effect_row.effects[i].idx != b.effect_row.effects[i].idx)
        return false;
    return true;
  case IPK_INT_VALUE:
    return a.int_value.type.v == b.int_value.type.v &&
           a.int_value.value == b.int_value.value;
  case IPK_FLOAT_VALUE: {
    uint64_t ab, bb;
    memcpy(&ab, &a.float_value.value, sizeof(ab));
    memcpy(&bb, &b.float_value.value, sizeof(bb));
    return a.float_value.type.v == b.float_value.type.v && ab == bb;
  }
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
    k.fn_type.n_params = p->n_params;
    k.fn_type.params = p->params; // stable for pool lifetime
    return k;
  }
  case IP_TAG_STRUCT_TYPE: {
    const IpStructPayload *p = arena_get_ptr(&pool->extra_arena, data);
    assert(p && "ip_key_internal: struct payload out of arena range");
    IpKey k = {.kind = IPK_STRUCT_TYPE};
    k.struct_type.zir_node_id = p->zir_node_id;
    k.struct_type.n_fields = p->n_fields;
    // The tail stores names (n_fields slots) then types (n_fields slots).
    // Both halves are u32-sized; StrId / IpIndex are u32-wrapper structs,
    // so the cast preserves layout.
    k.struct_type.field_names = (const StrId *)(p->tail);
    k.struct_type.field_types = (const IpIndex *)(p->tail + p->n_fields);
    return k;
  }
  case IP_TAG_ENUM_TYPE: {
    const IpEnumPayload *p = arena_get_ptr(&pool->extra_arena, data);
    assert(p && "ip_key_internal: enum header out of arena range");
    IpKey k = {.kind = IPK_ENUM_TYPE};
    k.enum_type.zir_node_id = p->zir_node_id;
    k.enum_type.n_variants = p->n_variants;
    k.enum_type.variant_names =
        (const StrId *)arena_get_ptr(&pool->extra_arena, p->names_offset);
    k.enum_type.variant_values =
        arena_get_ptr(&pool->extra_arena, p->values_offset);
    return k;
  }
  case IP_TAG_EFFECT_ROW: {
    const IpEffectRowPayload *p = arena_get_ptr(&pool->extra_arena, data);
    assert(p && "ip_key_internal: effect_row payload out of arena range");
    IpKey k = {.kind = IPK_EFFECT_ROW};
    k.effect_row.n_effects = p->n_effects;
    k.effect_row.effects = p->effects;
    return k;
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
    return true;
  default:
    return false;
  }
}

bool ip_is_value(InternPool *pool, IpIndex idx) {
  IpTag t = ip_tag(pool, idx);
  return t == IP_TAG_RESERVED_VALUE || t == IP_TAG_INT_VALUE ||
         t == IP_TAG_FLOAT_VALUE;
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
    p->n_params = (uint32_t)n;
    if (n > 0)
      memcpy(p->params, key.fn_type.params, n * sizeof(IpIndex));
    return off;
  }
  case IP_TAG_STRUCT_TYPE: {
    size_t n = key.struct_type.n_fields;
    // Storage is `uint32_t tail[]` regardless of typed view: names
    // (StrId, 4B) then types (IpIndex, 4B) packed back-to-back.
    size_t sz = sizeof(IpStructPayload) + 2 * n * sizeof(uint32_t);
    uint32_t off = (uint32_t)arena_total_used(&pool->extra_arena);
    IpStructPayload *p = arena_alloc_raw(&pool->extra_arena, sz);
    p->zir_node_id = key.struct_type.zir_node_id;
    p->n_fields = (uint32_t)n;
    if (n > 0) {
      memcpy(p->tail, key.struct_type.field_names, n * sizeof(StrId));
      memcpy(p->tail + n, key.struct_type.field_types, n * sizeof(IpIndex));
    }
    return off;
  }
  case IP_TAG_ENUM_TYPE: {
    size_t n = key.enum_type.n_variants;
    uint32_t header_off = (uint32_t)arena_total_used(&pool->extra_arena);
    IpEnumPayload *p = arena_alloc_raw(&pool->extra_arena, sizeof(*p));
    p->zir_node_id = key.enum_type.zir_node_id;
    p->n_variants = (uint32_t)n;

    // names[]: StrId array (u32-sized).
    p->names_offset = (uint32_t)arena_total_used(&pool->extra_arena);
    if (n > 0) {
      StrId *names = arena_alloc_raw(&pool->extra_arena, n * sizeof(StrId));
      memcpy(names, key.enum_type.variant_names, n * sizeof(StrId));
    }

    // values[]: i64 array. arena_alloc_raw aligns to 8 bytes so this
    // is well-formed for int64 reads.
    p->values_offset = (uint32_t)arena_total_used(&pool->extra_arena);
    if (n > 0) {
      int64_t *values =
          arena_alloc_raw(&pool->extra_arena, n * sizeof(int64_t));
      memcpy(values, key.enum_type.variant_values, n * sizeof(int64_t));
    }
    return header_off;
  }
  case IP_TAG_EFFECT_ROW: {
    size_t n = key.effect_row.n_effects;
    size_t sz = sizeof(IpEffectRowPayload) + n * sizeof(DefId);
    uint32_t off = (uint32_t)arena_total_used(&pool->extra_arena);
    IpEffectRowPayload *p = arena_alloc_raw(&pool->extra_arena, sz);
    p->n_effects = (uint32_t)n;
    if (n > 0)
      memcpy(p->effects, key.effect_row.effects, n * sizeof(DefId));
    return off;
  }
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
  case IPK_INT_VALUE:
    return IP_TAG_INT_VALUE;
  case IPK_FLOAT_VALUE:
    return IP_TAG_FLOAT_VALUE;
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
  {
    uint32_t off = (uint32_t)arena_total_used(&pool->extra_arena);
    IpEffectRowPayload *p = arena_alloc_raw(&pool->extra_arena, sizeof(*p));
    p->n_effects = 0;
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

  populate_reserved(pool);
}

void ip_free(InternPool *pool) {
  free(pool->items_tag);
  free(pool->items_data);
  free(pool->buckets);
  arena_free(&pool->extra_arena);
  memset(pool, 0, sizeof(*pool));
}

// =====================================================================
// WipContainer — two-phase construction.
// =====================================================================

WipContainerType ip_wip_struct(InternPool *pool, uint32_t zir_node_id,
                               const IpIndex *captures, size_t n_captures) {
  (void)captures;
  (void)n_captures; // Generics groundwork — not yet dispatched.

  // Allocate a placeholder payload (n_fields == 0).
  uint32_t off = (uint32_t)arena_total_used(&pool->extra_arena);
  IpStructPayload *p = arena_alloc_raw(&pool->extra_arena, sizeof(*p));
  p->zir_node_id = zir_node_id;
  p->n_fields = 0;

  uint32_t idx = append_item(pool, IP_TAG_STRUCT_TYPE, off);

  // Register in the dedup map by zir_node_id (identity).
  IpKey k = {.kind = IPK_STRUCT_TYPE};
  k.struct_type.zir_node_id = zir_node_id;
  uint64_t h = hash_key(k);
  uint32_t hh = (uint32_t)(h >> 32);
  if ((pool->bucket_used + 1) * 4 > pool->bucket_count * 3)
    buckets_grow(pool);
  size_t mask = pool->bucket_count - 1;
  size_t b = (size_t)(h & mask);
  while (pool->buckets[b] != 0)
    b = (b + 1) & mask;
  pool->buckets[b] = ((uint64_t)hh << 32) | (uint64_t)(idx + 1);
  pool->bucket_used++;

  return (WipContainerType){.index = (IpIndex){idx}, .reserved = 0};
}

void ip_wip_struct_finish(InternPool *pool, WipContainerType wip,
                          const StrId *field_names, const IpIndex *field_types,
                          size_t n_fields) {
  // Allocate a FRESH payload. This is what makes _finish safe to call
  // after arbitrary intervening ip_get / arena allocations: we don't
  // depend on the wip's original payload being the latest arena tail.
  // The original payload becomes ~12 bytes of arena garbage.
  uint32_t off = (uint32_t)arena_total_used(&pool->extra_arena);
  size_t sz = sizeof(IpStructPayload) + 2 * n_fields * sizeof(uint32_t);
  IpStructPayload *p = arena_alloc_raw(&pool->extra_arena, sz);

  // Preserve the original zir_node_id from the wip stub.
  IpStructPayload *orig =
      arena_get_ptr(&pool->extra_arena, pool->items_data[wip.index.v]);
  assert(orig && "ip_wip_struct_finish: wip stub payload missing");
  p->zir_node_id = orig->zir_node_id;
  p->n_fields = (uint32_t)n_fields;
  if (n_fields > 0) {
    memcpy(p->tail, field_names, n_fields * sizeof(StrId));
    memcpy(p->tail + n_fields, field_types, n_fields * sizeof(IpIndex));
  }

  pool->items_data[wip.index.v] = off;
}

void ip_wip_struct_cancel(InternPool *pool, WipContainerType wip) {
  pool->items_tag[wip.index.v] = IP_TAG_REMOVED;
  // Bucket entry leaks per the mark-removed contract.
}

WipContainerType ip_wip_fn_type(InternPool *pool, uint32_t modifiers,
                                size_t n_params) {
  // Placeholder payload — ret/params filled in by _finish.
  size_t sz = sizeof(IpFnPayload) + n_params * sizeof(IpIndex);
  uint32_t off = (uint32_t)arena_total_used(&pool->extra_arena);
  IpFnPayload *p = arena_alloc_raw(&pool->extra_arena, sz);
  p->ret = IP_NONE;
  p->modifiers = modifiers;
  p->n_params = (uint32_t)n_params;
  for (size_t i = 0; i < n_params; i++)
    p->params[i] = IP_NONE;

  uint32_t idx = append_item(pool, IP_TAG_FN_TYPE, off);

  // Fn types have structural identity. The wip placeholder isn't
  // structurally complete yet, so we don't register it in the bucket
  // map here — _finish handles registration with the final shape.

  return (WipContainerType){.index = (IpIndex){idx}, .reserved = 0};
}

void ip_wip_fn_finish(InternPool *pool, WipContainerType wip, IpIndex ret,
                      uint32_t modifiers, const IpIndex *params,
                      size_t n_params) {
  // Allocate fresh payload (same rationale as struct _finish).
  uint32_t off = (uint32_t)arena_total_used(&pool->extra_arena);
  size_t sz = sizeof(IpFnPayload) + n_params * sizeof(IpIndex);
  IpFnPayload *p = arena_alloc_raw(&pool->extra_arena, sz);
  p->ret = ret;
  p->modifiers = modifiers;
  p->n_params = (uint32_t)n_params;
  if (n_params > 0)
    memcpy(p->params, params, n_params * sizeof(IpIndex));

  pool->items_data[wip.index.v] = off;

  // Register in bucket map with the final structural identity.
  IpKey k = {.kind = IPK_FN_TYPE};
  k.fn_type.ret = ret;
  k.fn_type.modifiers = modifiers;
  k.fn_type.params = params;
  k.fn_type.n_params = n_params;

  uint64_t h = hash_key(k);
  uint32_t hh = (uint32_t)(h >> 32);
  if ((pool->bucket_used + 1) * 4 > pool->bucket_count * 3)
    buckets_grow(pool);
  size_t mask = pool->bucket_count - 1;
  size_t b = (size_t)(h & mask);
  while (pool->buckets[b] != 0)
    b = (b + 1) & mask;
  pool->buckets[b] = ((uint64_t)hh << 32) | (uint64_t)(wip.index.v + 1);
  pool->bucket_used++;
}

void ip_wip_fn_cancel(InternPool *pool, WipContainerType wip) {
  pool->items_tag[wip.index.v] = IP_TAG_REMOVED;
}

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
  fprintf(out, "  effect_row          %zu\n", per_tag[IP_TAG_EFFECT_ROW]);
  fprintf(out, "  int_value           %zu\n", per_tag[IP_TAG_INT_VALUE]);
  fprintf(out, "  float_value         %zu\n", per_tag[IP_TAG_FLOAT_VALUE]);
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
// Indexed by IpReservedIndex's primitive range (0 .. IP_INDEX_BOOL_TRUE-1).
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
    if (v < (uint32_t)IP_INDEX_BOOL_TRUE) {
      fb_puts(fb, ip_primitive_names[v]);
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
    fb_puts(fb, ") -> ");
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
    for (size_t i = 0; i < k.effect_row.n_effects; i++) {
      if (i > 0)
        fb_puts(fb, ", ");
      fb_puts(fb, "def#");
      fb_putu(fb, k.effect_row.effects[i].idx);
    }
    fb_putc(fb, '>');
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
