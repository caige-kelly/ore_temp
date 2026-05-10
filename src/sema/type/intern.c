#include "intern.h"

#include <string.h>

#include "../../common/arena.h"
#include "../../common/hashmap.h"
#include "../../common/vec.h"
#include "../query/query_engine.h"
#include "../sema.h"
#include "type.h"

// Compound-type interning.
//
// Each shape gets an FNV-1a content hash → Type* hashmap. Lookup
// walks the bucket and checks structural equality before returning;
// hash collisions don't yield false positives. Storage is on Sema:
//
//   Sema.fn_types     fn(params...) -> ret
//   Sema.ptr_types    ^T  /  ^const T
//   Sema.slice_types  []T / []const T
//   Sema.array_types  [N]T
//
// The hashmaps store Vec*<Type*> per bucket so collision-walking
// stays cheap. With the small number of distinct types in any
// realistic program (tens), collisions are rare.

void sema_type_interns_init(struct Sema *s) {
  if (!s)
    return;
  if (s->fn_types.entries == NULL)
    hashmap_init_in(&s->fn_types, &s->arena);
  if (s->ptr_types.entries == NULL)
    hashmap_init_in(&s->ptr_types, &s->arena);
  if (s->many_ptr_types.entries == NULL)
    hashmap_init_in(&s->many_ptr_types, &s->arena);
  if (s->slice_types.entries == NULL)
    hashmap_init_in(&s->slice_types, &s->arena);
  if (s->array_types.entries == NULL)
    hashmap_init_in(&s->array_types, &s->arena);
  if (s->optional_types.entries == NULL)
    hashmap_init_in(&s->optional_types, &s->arena);
  if (s->struct_types.entries == NULL)
    hashmap_init_in(&s->struct_types, &s->arena);
  if (s->enum_types.entries == NULL)
    hashmap_init_in(&s->enum_types, &s->arena);
}

// === Hash helpers ===

static uint64_t hash_ptr(const void *p) {
  return query_fingerprint_from_pointer(p);
}

static uint64_t hash_fn(struct Type **params, size_t n, struct Type *ret) {
  uint64_t h = hash_ptr(ret);
  for (size_t i = 0; i < n; i++)
    h = query_fingerprint_combine(h, hash_ptr(params[i]));
  // Mix in count so fn(a) and fn(a, a) hash differently when a==a.
  return query_fingerprint_combine(h, query_fingerprint_from_u64(n));
}

static uint64_t hash_ptr_or_slice(struct Type *elem, bool is_const) {
  return query_fingerprint_combine(hash_ptr(elem),
                                   query_fingerprint_from_u64(is_const));
}

static uint64_t hash_array(struct Type *elem, uint64_t size) {
  return query_fingerprint_combine(hash_ptr(elem),
                                   query_fingerprint_from_u64(size));
}

// === Bucket lookup helpers ===
//
// Each interner stores `Vec<struct Type*>*` per hash bucket. On a
// hit, walk the vec and structural-compare. On a miss, allocate a
// new Type, append to the bucket (or create the bucket).

static Vec *bucket_for(HashMap *map, Arena *arena, uint64_t key) {
  if (hashmap_contains(map, key))
    return (Vec *)hashmap_get(map, key);
  Vec *v = vec_new_in(arena, sizeof(struct Type *));
  hashmap_put_or_die(map, key, v, "type_intern_bucket");
  return v;
}

// R4 Step 3 — register a freshly-allocated pool-managed Type* in
// the IpIndex → Type* bridge table. Grows types_by_ip as needed.
static void ip_bridge_register(struct Sema *s, IpIndex idx, struct Type *t) {
  while (s->types_by_ip->count <= idx.v) {
    struct Type *null_p = NULL;
    vec_push(s->types_by_ip, &null_p);
  }
  struct Type **slot = (struct Type **)vec_get(s->types_by_ip, idx.v);
  *slot = t;
}

static struct Type *append_new(Arena *arena, Vec *bucket, struct Type proto) {
  struct Type *t = arena_alloc(arena, sizeof(struct Type));
  *t = proto;
  vec_push(bucket, &t);
  return t;
}

// === Public constructors ===

struct Type *type_fn(struct Sema *s, struct Type **params, size_t param_count,
                     struct Type *ret) {
  if (!s || !ret)
    return NULL;

  // R4 Step 3f — pool-driven identity when ret and every param have
  // valid IpIndex. Variable-length params packed into `extra` by the
  // pool itself; we just hand it an IpIndex array borrowed for the
  // duration of ip_get. Stack buffer for the common-case (<= 16
  // params); arena fallback for the rare large signature.
  bool all_pool_managed = ip_index_is_valid(ret->ip);
  for (size_t i = 0; i < param_count && all_pool_managed; i++) {
    if (!params[i] || !ip_index_is_valid(params[i]->ip))
      all_pool_managed = false;
  }

  if (all_pool_managed) {
    IpIndex stack_ips[16];
    IpIndex *ips = stack_ips;
    if (param_count > 16) {
      ips = arena_alloc(&s->arena, sizeof(IpIndex) * param_count);
    }
    for (size_t i = 0; i < param_count; i++)
      ips[i] = params[i]->ip;

    IpKey key = {.kind = IPK_FN_TYPE};
    key.fn_type.ret = ret->ip;
    key.fn_type.modifiers = 0;
    key.fn_type.params = ips;
    key.fn_type.n_params = param_count;
    IpIndex idx = ip_get(&s->intern_pool, key);

    struct Type *existing = type_of_ip(s, idx);
    if (existing) return existing;

    // First sighting — allocate the Type* and copy params into the
    // arena (pool's extra is for IpIndex storage; consumers of
    // struct Type * still want the legacy Type** array).
    struct Type **owned_params = NULL;
    if (param_count > 0) {
      owned_params = arena_alloc(&s->arena, sizeof(struct Type *) * param_count);
      memcpy(owned_params, params, sizeof(struct Type *) * param_count);
    }
    struct Type *t = arena_alloc(&s->arena, sizeof(struct Type));
    t->kind = TY_FN;
    t->ip = idx;
    t->fn.params = owned_params;
    t->fn.param_count = param_count;
    t->fn.ret = ret;
    ip_bridge_register(s, idx, t);
    return t;
  }

  // Legacy bucket fallback (used when struct/enum types appear as
  // params or ret — they haven't been pool-migrated yet in Step 3f).
  uint64_t h = hash_fn(params, param_count, ret);
  Vec *bucket = bucket_for(&s->fn_types, &s->arena, h);

  for (size_t i = 0; i < bucket->count; i++) {
    struct Type **slot = (struct Type **)vec_get(bucket, i);
    struct Type *t = slot ? *slot : NULL;
    if (!t || t->kind != TY_FN)
      continue;
    if (t->fn.ret != ret || t->fn.param_count != param_count)
      continue;
    bool same = true;
    for (size_t j = 0; j < param_count; j++) {
      if (t->fn.params[j] != params[j]) {
        same = false;
        break;
      }
    }
    if (same)
      return t;
  }

  struct Type **owned_params = NULL;
  if (param_count > 0) {
    owned_params = arena_alloc(&s->arena, sizeof(struct Type *) * param_count);
    memcpy(owned_params, params, sizeof(struct Type *) * param_count);
  }
  struct Type proto = {.kind = TY_FN};
  proto.fn.params = owned_params;
  proto.fn.param_count = param_count;
  proto.fn.ret = ret;
  return append_new(&s->arena, bucket, proto);
}

// R4 Step 3a-e — single-element compound type constructors. All four
// (ptr, many_ptr, slice, optional) plus array follow the same shape:
//
//   if elem has a valid IpIndex (pool-managed):
//     - build the IpKey
//     - ip_get → IpIndex
//     - check bridge for existing backing Type*; reuse if present
//     - else allocate Type*, register, return
//   else:
//     - fall back to the legacy bucket
//
// The fallback path becomes dead once Step 3f-g land (every compound
// will have a pool-assigned ip).

struct Type *type_ptr(struct Sema *s, struct Type *elem, bool is_const) {
  if (!s || !elem)
    return NULL;
  if (ip_index_is_valid(elem->ip)) {
    IpKey key = {.kind = IPK_PTR_TYPE};
    key.ptr_type.elem = elem->ip;
    key.ptr_type.is_const = is_const;
    IpIndex idx = ip_get(&s->intern_pool, key);
    struct Type *existing = type_of_ip(s, idx);
    if (existing) return existing;
    struct Type *t = arena_alloc(&s->arena, sizeof(struct Type));
    t->kind = TY_PTR;
    t->ip = idx;
    t->ptr.elem = elem;
    t->ptr.is_const = is_const;
    ip_bridge_register(s, idx, t);
    return t;
  }
  uint64_t h = hash_ptr_or_slice(elem, is_const);
  Vec *bucket = bucket_for(&s->ptr_types, &s->arena, h);
  for (size_t i = 0; i < bucket->count; i++) {
    struct Type **slot = (struct Type **)vec_get(bucket, i);
    struct Type *t = slot ? *slot : NULL;
    if (!t || t->kind != TY_PTR)
      continue;
    if (t->ptr.elem == elem && t->ptr.is_const == is_const)
      return t;
  }
  struct Type proto = {.kind = TY_PTR};
  proto.ptr.elem = elem;
  proto.ptr.is_const = is_const;
  return append_new(&s->arena, bucket, proto);
}

struct Type *type_many_ptr(struct Sema *s, struct Type *elem, bool is_const) {
  if (!s || !elem)
    return NULL;
  if (ip_index_is_valid(elem->ip)) {
    IpKey key = {.kind = IPK_MANY_PTR_TYPE};
    key.many_ptr_type.elem = elem->ip;
    key.many_ptr_type.is_const = is_const;
    IpIndex idx = ip_get(&s->intern_pool, key);
    struct Type *existing = type_of_ip(s, idx);
    if (existing) return existing;
    struct Type *t = arena_alloc(&s->arena, sizeof(struct Type));
    t->kind = TY_MANY_PTR;
    t->ip = idx;
    t->many_ptr.elem = elem;
    t->many_ptr.is_const = is_const;
    ip_bridge_register(s, idx, t);
    return t;
  }
  uint64_t h = hash_ptr_or_slice(elem, is_const);
  Vec *bucket = bucket_for(&s->many_ptr_types, &s->arena, h);
  for (size_t i = 0; i < bucket->count; i++) {
    struct Type **slot = (struct Type **)vec_get(bucket, i);
    struct Type *t = slot ? *slot : NULL;
    if (!t || t->kind != TY_MANY_PTR)
      continue;
    if (t->many_ptr.elem == elem && t->many_ptr.is_const == is_const)
      return t;
  }
  struct Type proto = {.kind = TY_MANY_PTR};
  proto.many_ptr.elem = elem;
  proto.many_ptr.is_const = is_const;
  return append_new(&s->arena, bucket, proto);
}

struct Type *type_slice(struct Sema *s, struct Type *elem, bool is_const) {
  if (!s || !elem)
    return NULL;
  if (ip_index_is_valid(elem->ip)) {
    IpKey key = {.kind = IPK_SLICE_TYPE};
    key.slice_type.elem = elem->ip;
    key.slice_type.is_const = is_const;
    IpIndex idx = ip_get(&s->intern_pool, key);
    struct Type *existing = type_of_ip(s, idx);
    if (existing) return existing;
    struct Type *t = arena_alloc(&s->arena, sizeof(struct Type));
    t->kind = TY_SLICE;
    t->ip = idx;
    t->slice.elem = elem;
    t->slice.is_const = is_const;
    ip_bridge_register(s, idx, t);
    return t;
  }
  uint64_t h = hash_ptr_or_slice(elem, is_const);
  Vec *bucket = bucket_for(&s->slice_types, &s->arena, h);
  for (size_t i = 0; i < bucket->count; i++) {
    struct Type **slot = (struct Type **)vec_get(bucket, i);
    struct Type *t = slot ? *slot : NULL;
    if (!t || t->kind != TY_SLICE)
      continue;
    if (t->slice.elem == elem && t->slice.is_const == is_const)
      return t;
  }
  struct Type proto = {.kind = TY_SLICE};
  proto.slice.elem = elem;
  proto.slice.is_const = is_const;
  return append_new(&s->arena, bucket, proto);
}

struct Type *type_optional(struct Sema *s, struct Type *elem) {
  if (!s || !elem)
    return NULL;
  // ?(?T) collapses to ?T — nesting optional doesn't add a level.
  // Mirrors Zig's "?T is canonical for any optional of T" treatment;
  // double-nesting was a Rust-style choice that doesn't fit the
  // value-or-nil semantics we want here.
  if (elem->kind == TY_OPTIONAL)
    return elem;

  if (ip_index_is_valid(elem->ip)) {
    IpKey key = {.kind = IPK_OPTIONAL_TYPE};
    key.optional_type.elem = elem->ip;
    IpIndex idx = ip_get(&s->intern_pool, key);
    struct Type *existing = type_of_ip(s, idx);
    if (existing) return existing;
    struct Type *t = arena_alloc(&s->arena, sizeof(struct Type));
    t->kind = TY_OPTIONAL;
    t->ip = idx;
    t->optional.elem = elem;
    ip_bridge_register(s, idx, t);
    return t;
  }
  uint64_t h = hash_ptr(elem);
  Vec *bucket = bucket_for(&s->optional_types, &s->arena, h);
  for (size_t i = 0; i < bucket->count; i++) {
    struct Type **slot = (struct Type **)vec_get(bucket, i);
    struct Type *t = slot ? *slot : NULL;
    if (!t || t->kind != TY_OPTIONAL)
      continue;
    if (t->optional.elem == elem)
      return t;
  }
  struct Type proto = {.kind = TY_OPTIONAL};
  proto.optional.elem = elem;
  return append_new(&s->arena, bucket, proto);
}

struct Type *type_array(struct Sema *s, struct Type *elem, uint64_t size) {
  if (!s || !elem)
    return NULL;
  if (ip_index_is_valid(elem->ip)) {
    IpKey key = {.kind = IPK_ARRAY_TYPE};
    key.array_type.elem = elem->ip;
    key.array_type.size = size;
    IpIndex idx = ip_get(&s->intern_pool, key);
    struct Type *existing = type_of_ip(s, idx);
    if (existing) return existing;
    struct Type *t = arena_alloc(&s->arena, sizeof(struct Type));
    t->kind = TY_ARRAY;
    t->ip = idx;
    t->array.elem = elem;
    t->array.size = size;
    ip_bridge_register(s, idx, t);
    return t;
  }
  uint64_t h = hash_array(elem, size);
  Vec *bucket = bucket_for(&s->array_types, &s->arena, h);
  for (size_t i = 0; i < bucket->count; i++) {
    struct Type **slot = (struct Type **)vec_get(bucket, i);
    struct Type *t = slot ? *slot : NULL;
    if (!t || t->kind != TY_ARRAY)
      continue;
    if (t->array.elem == elem && t->array.size == size)
      return t;
  }
  struct Type proto = {.kind = TY_ARRAY};
  proto.array.elem = elem;
  proto.array.size = size;
  return append_new(&s->arena, bucket, proto);
}

// Nominal type interners. The type's identity is the DefId itself, so
// we key the hashmap directly by DefId.idx — no structural-equal walk,
// no bucket vec. First call for a given def allocates the Type;
// subsequent calls return the same pointer. This is the interning
// invariant downstream code relies on (pointer-equality = type-equality).

// R4 Step 3g — TY_STRUCT and TY_ENUM are nominal: identity is the
// DefId of the declaration (mapped to the pool's `zir_node_id` field,
// same semantics — a stable per-source-decl identity). Fields and
// variants live in StructSignature / EnumSignature side tables; the
// Type itself carries only identity, so we route through ip_get with
// n_fields = 0 / n_variants = 0 rather than the WipContainer two-
// phase API. WipContainer is groundwork for the future case where
// fields might need to live in the pool (e.g., when the pool becomes
// the source of truth for layout) — for now identity alone suffices.

struct Type *type_struct(struct Sema *s, DefId def) {
  if (!s || !def_id_is_valid(def))
    return NULL;
  IpKey key = {.kind = IPK_STRUCT_TYPE};
  key.struct_type.zir_node_id = def.idx;
  key.struct_type.n_fields = 0;
  key.struct_type.field_names = NULL;
  key.struct_type.field_types = NULL;
  IpIndex idx = ip_get(&s->intern_pool, key);

  struct Type *existing = type_of_ip(s, idx);
  if (existing) return existing;

  struct Type *t = arena_alloc(&s->arena, sizeof(*t));
  *t = (struct Type){.kind = TY_STRUCT};
  t->ip = idx;
  t->struct_.def = def;
  ip_bridge_register(s, idx, t);
  return t;
}

struct Type *type_enum(struct Sema *s, DefId def) {
  if (!s || !def_id_is_valid(def))
    return NULL;
  IpKey key = {.kind = IPK_ENUM_TYPE};
  key.enum_type.zir_node_id = def.idx;
  key.enum_type.n_variants = 0;
  key.enum_type.variant_names = NULL;
  key.enum_type.variant_values = NULL;
  IpIndex idx = ip_get(&s->intern_pool, key);

  struct Type *existing = type_of_ip(s, idx);
  if (existing) return existing;

  struct Type *t = arena_alloc(&s->arena, sizeof(*t));
  *t = (struct Type){.kind = TY_ENUM};
  t->ip = idx;
  t->enum_.def = def;
  ip_bridge_register(s, idx, t);
  return t;
}
