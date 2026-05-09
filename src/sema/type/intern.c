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
  if (!s) return;
  if (s->fn_types.entries == NULL)
    hashmap_init_in(&s->fn_types, &s->arena);
  if (s->ptr_types.entries == NULL)
    hashmap_init_in(&s->ptr_types, &s->arena);
  if (s->slice_types.entries == NULL)
    hashmap_init_in(&s->slice_types, &s->arena);
  if (s->array_types.entries == NULL)
    hashmap_init_in(&s->array_types, &s->arena);
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

static struct Type *append_new(Arena *arena, Vec *bucket, struct Type proto) {
  struct Type *t = arena_alloc(arena, sizeof(struct Type));
  *t = proto;
  vec_push(bucket, &t);
  return t;
}

// === Public constructors ===

struct Type *type_fn(struct Sema *s, struct Type **params, size_t param_count,
                     struct Type *ret) {
  if (!s || !ret) return NULL;
  uint64_t h = hash_fn(params, param_count, ret);
  Vec *bucket = bucket_for(&s->fn_types, &s->arena, h);

  for (size_t i = 0; i < bucket->count; i++) {
    struct Type **slot = (struct Type **)vec_get(bucket, i);
    struct Type *t = slot ? *slot : NULL;
    if (!t || t->kind != TY_FN) continue;
    if (t->fn.ret != ret || t->fn.param_count != param_count) continue;
    bool same = true;
    for (size_t j = 0; j < param_count; j++) {
      if (t->fn.params[j] != params[j]) { same = false; break; }
    }
    if (same) return t;
  }

  // Miss: allocate a fresh fn type. Copy params into the arena so
  // callers can stack-allocate or reuse their array.
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

struct Type *type_ptr(struct Sema *s, struct Type *elem, bool is_const) {
  if (!s || !elem) return NULL;
  uint64_t h = hash_ptr_or_slice(elem, is_const);
  Vec *bucket = bucket_for(&s->ptr_types, &s->arena, h);

  for (size_t i = 0; i < bucket->count; i++) {
    struct Type **slot = (struct Type **)vec_get(bucket, i);
    struct Type *t = slot ? *slot : NULL;
    if (!t || t->kind != TY_PTR) continue;
    if (t->ptr.elem == elem && t->ptr.is_const == is_const) return t;
  }

  struct Type proto = {.kind = TY_PTR};
  proto.ptr.elem = elem;
  proto.ptr.is_const = is_const;
  return append_new(&s->arena, bucket, proto);
}

struct Type *type_slice(struct Sema *s, struct Type *elem, bool is_const) {
  if (!s || !elem) return NULL;
  uint64_t h = hash_ptr_or_slice(elem, is_const);
  Vec *bucket = bucket_for(&s->slice_types, &s->arena, h);

  for (size_t i = 0; i < bucket->count; i++) {
    struct Type **slot = (struct Type **)vec_get(bucket, i);
    struct Type *t = slot ? *slot : NULL;
    if (!t || t->kind != TY_SLICE) continue;
    if (t->slice.elem == elem && t->slice.is_const == is_const) return t;
  }

  struct Type proto = {.kind = TY_SLICE};
  proto.slice.elem = elem;
  proto.slice.is_const = is_const;
  return append_new(&s->arena, bucket, proto);
}

struct Type *type_array(struct Sema *s, struct Type *elem, uint64_t size) {
  if (!s || !elem) return NULL;
  uint64_t h = hash_array(elem, size);
  Vec *bucket = bucket_for(&s->array_types, &s->arena, h);

  for (size_t i = 0; i < bucket->count; i++) {
    struct Type **slot = (struct Type **)vec_get(bucket, i);
    struct Type *t = slot ? *slot : NULL;
    if (!t || t->kind != TY_ARRAY) continue;
    if (t->array.elem == elem && t->array.size == size) return t;
  }

  struct Type proto = {.kind = TY_ARRAY};
  proto.array.elem = elem;
  proto.array.size = size;
  return append_new(&s->arena, bucket, proto);
}
