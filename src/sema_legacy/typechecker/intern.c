#include "intern.h"

#include <assert.h>
#include <string.h>

#include "support/data_structure/arena.h"
#include "support/data_structure/vec.h"
#include "../sema.h"
#include "type.h"

// Compound-type interning.
//
// Post-R4 cleanup, every `type_*` constructor routes through the
// unified intern pool. Identity = IpIndex; the `struct Type *` we
// return is a bridge object kept on Sema.types_by_ip[idx.v]. Two
// calls with structurally-identical input get the same Type* via
// the pool's dedup, then the bridge serves the same backing
// allocation.
//
// The pool requires every elem / param / ret input to have a valid
// `ip`. Primitives get theirs hooked at sema_types_init time. Other
// compounds get theirs assigned when their constructor runs. The
// asserts below enforce the contract at runtime — if any caller
// passes a Type* without an ip, we surface it immediately rather
// than silently producing a malformed pool entry.
//
// Pre-cleanup this file held ~250 LoC of legacy bucket-based
// hashmaps (`fn_types`, `ptr_types`, etc.) used as fallback during
// the Steps 3a-g migration. All dead and deleted.

// Register a freshly-allocated pool-managed Type* in the IpIndex →
// Type* bridge table. Grows types_by_ip as needed.
static void ip_bridge_register(struct Sema *s, IpIndex idx, struct Type *t) {
  while (s->types_by_ip->count <= idx.v) {
    struct Type *null_p = NULL;
    vec_push(s->types_by_ip, &null_p);
  }
  struct Type **slot = (struct Type **)vec_get(s->types_by_ip, idx.v);
  *slot = t;
}

// === Public constructors ===

struct Type *type_fn(struct Sema *s, struct Type **params, size_t param_count,
                     struct Type *ret) {
  if (!s || !ret)
    return NULL;
  assert(ip_index_is_valid(ret->ip) && "type_fn: ret has no IpIndex");

  // Stack buffer for the common case (<= 16 params); arena fallback
  // for the rare large signature.
  IpIndex stack_ips[16];
  IpIndex *ips = stack_ips;
  if (param_count > 16)
    ips = arena_alloc(&s->arena, sizeof(IpIndex) * param_count);
  for (size_t i = 0; i < param_count; i++) {
    assert(params[i] && ip_index_is_valid(params[i]->ip) &&
           "type_fn: param has no IpIndex");
    ips[i] = params[i]->ip;
  }

  IpKey key = {.kind = IPK_FN_TYPE};
  key.fn_type.ret = ret->ip;
  key.fn_type.modifiers = 0;
  key.fn_type.params = ips;
  key.fn_type.n_params = param_count;
  IpIndex idx = ip_get(&s->intern_pool, key);

  struct Type *existing = type_of_ip(s, idx);
  if (existing)
    return existing;

  // Copy params into the arena — consumers of struct Type * want
  // a stable Type** array. Pool stores IpIndex values in `extra`
  // independently.
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

struct Type *type_ptr(struct Sema *s, struct Type *elem, bool is_const) {
  if (!s || !elem)
    return NULL;
  assert(ip_index_is_valid(elem->ip) && "type_ptr: elem has no IpIndex");

  IpKey key = {.kind = IPK_PTR_TYPE};
  key.ptr_type.elem = elem->ip;
  key.ptr_type.is_const = is_const;
  IpIndex idx = ip_get(&s->intern_pool, key);

  struct Type *existing = type_of_ip(s, idx);
  if (existing)
    return existing;

  struct Type *t = arena_alloc(&s->arena, sizeof(struct Type));
  t->kind = TY_PTR;
  t->ip = idx;
  t->ptr.elem = elem;
  t->ptr.is_const = is_const;
  ip_bridge_register(s, idx, t);
  return t;
}

struct Type *type_many_ptr(struct Sema *s, struct Type *elem, bool is_const) {
  if (!s || !elem)
    return NULL;
  assert(ip_index_is_valid(elem->ip) && "type_many_ptr: elem has no IpIndex");

  IpKey key = {.kind = IPK_MANY_PTR_TYPE};
  key.many_ptr_type.elem = elem->ip;
  key.many_ptr_type.is_const = is_const;
  IpIndex idx = ip_get(&s->intern_pool, key);

  struct Type *existing = type_of_ip(s, idx);
  if (existing)
    return existing;

  struct Type *t = arena_alloc(&s->arena, sizeof(struct Type));
  t->kind = TY_MANY_PTR;
  t->ip = idx;
  t->many_ptr.elem = elem;
  t->many_ptr.is_const = is_const;
  ip_bridge_register(s, idx, t);
  return t;
}

struct Type *type_slice(struct Sema *s, struct Type *elem, bool is_const) {
  if (!s || !elem)
    return NULL;
  assert(ip_index_is_valid(elem->ip) && "type_slice: elem has no IpIndex");

  IpKey key = {.kind = IPK_SLICE_TYPE};
  key.slice_type.elem = elem->ip;
  key.slice_type.is_const = is_const;
  IpIndex idx = ip_get(&s->intern_pool, key);

  struct Type *existing = type_of_ip(s, idx);
  if (existing)
    return existing;

  struct Type *t = arena_alloc(&s->arena, sizeof(struct Type));
  t->kind = TY_SLICE;
  t->ip = idx;
  t->slice.elem = elem;
  t->slice.is_const = is_const;
  ip_bridge_register(s, idx, t);
  return t;
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
  assert(ip_index_is_valid(elem->ip) && "type_optional: elem has no IpIndex");

  IpKey key = {.kind = IPK_OPTIONAL_TYPE};
  key.optional_type.elem = elem->ip;
  IpIndex idx = ip_get(&s->intern_pool, key);

  struct Type *existing = type_of_ip(s, idx);
  if (existing)
    return existing;

  struct Type *t = arena_alloc(&s->arena, sizeof(struct Type));
  t->kind = TY_OPTIONAL;
  t->ip = idx;
  t->optional.elem = elem;
  ip_bridge_register(s, idx, t);
  return t;
}

struct Type *type_array(struct Sema *s, struct Type *elem, uint64_t size) {
  if (!s || !elem)
    return NULL;
  assert(ip_index_is_valid(elem->ip) && "type_array: elem has no IpIndex");

  IpKey key = {.kind = IPK_ARRAY_TYPE};
  key.array_type.elem = elem->ip;
  key.array_type.size = size;
  IpIndex idx = ip_get(&s->intern_pool, key);

  struct Type *existing = type_of_ip(s, idx);
  if (existing)
    return existing;

  struct Type *t = arena_alloc(&s->arena, sizeof(struct Type));
  t->kind = TY_ARRAY;
  t->ip = idx;
  t->array.elem = elem;
  t->array.size = size;
  ip_bridge_register(s, idx, t);
  return t;
}

// Nominal type interners. The type's identity is the DefId itself,
// mapped to the pool's `zir_node_id` field — same semantics, a
// stable per-source-decl identity. Fields and variants live in
// StructSignature / EnumSignature side tables; the Type itself
// carries only identity, so we route through ip_get with
// n_fields = 0 / n_variants = 0 rather than the WipContainer API.

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
  if (existing)
    return existing;

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
  if (existing)
    return existing;

  struct Type *t = arena_alloc(&s->arena, sizeof(*t));
  *t = (struct Type){.kind = TY_ENUM};
  t->ip = idx;
  t->enum_.def = def;
  ip_bridge_register(s, idx, t);
  return t;
}
