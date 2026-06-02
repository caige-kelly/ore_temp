// Unit tests for the unified intern pool.
//
// Covers:
//   T1  — ip_init populates reserved indices at their expected slots
//   T2  — Reserved-index fast path: ip_get on a primitive returns the
//         pre-baked IpIndex without touching the bucket map
//   T3  — Round-trip identity: ip_key(ip_get(k)) reproduces k
//   T4  — Dedup: two ip_get calls with structurally-equal keys return
//         the same IpIndex
//   T5  — Variant dedup across kinds: a ptr_type and a slice_type with
//         the same elem are distinct IpIndices
//   T6  — Constness discrimination: `^T` vs `^const T` are distinct
//   T7  — Array dedup is sensitive to size
//   T8  — Fn-type dedup handles variable-length params correctly
//   T9  — Int values dedup on (type, value) pair
//   T10 — ip_remove marks the slot; subsequent ip_get on the same key
//         allocates a fresh index
//   T11 — WipContainer round-trip: allocate, use index, finish, ip_key
//         returns the populated fields
//   T12 — WipContainer cancel marks removed and lets a fresh ip_get of
//         the same zir_node_id succeed
//   T13 — Arena stability: borrowed pointers from ip_key remain valid
//         across many subsequent ip_get calls (chunk growth + items
//         realloc + bucket grow)
//   T14 — Effect-row dedup canonicalization: same effects from different
//         source arrays return the same IpIndex; empty row hits the
//         reserved IP_EMPTY_EFFECT_ROW fast path
//   T15 — Reserved compound fast-path identity: ip_get for `^const u8`,
//         `^void`, `^const void`, `[]const u8` return the pre-baked
//         IpIndex constants without allocating new items
//   T16 — Nominal inline identity (D2.1b): struct/enum types carry only
//         their zir_node_id, inline-encoded — ip_get is stable + deduped by
//         zir (re-get → same index), ip_key round-trips the zir, distinct
//         zirs are distinct types. Field/variant lists live in the db pools.
//   T17 — ip_wip_fn_type round trip: self-referential fn type using
//         ^Self as a param resolves correctly (structural fns still use wip)
//   T18 — ip_init_with: custom initial bucket and extra-arena sizing
//         survives the same workload as default init
//   T19 — ip_clear: populating then clearing returns the pool to a
//         fresh-init state; user items gone, reserved items intact
//   T20 — ip_format: textual representation of common compound types
//         matches the documented examples
//
// Run via `make test-intern-pool`.

#include "../src/db/intern_pool/intern_pool.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;
static const char *g_current = NULL;

static void start(const char *name) {
  g_current = name;
  printf("  ... %s\n", name);
}
static void finish(bool ok) {
  if (ok) {
    g_pass++;
  } else {
    g_fail++;
    fprintf(stderr, "       FAIL: %s\n", g_current);
  }
}

// =====================================================================

static void test_reserved_indices(void) {
  start("ip_init populates reserved indices at stable slots");
  InternPool pool;
  ip_init(&pool);

  bool ok = true;
  // Primitives by enum position.
  ok &= ip_index_eq(IP_BOOL_TYPE,  (IpIndex){IP_INDEX_BOOL_TYPE});
  ok &= ip_index_eq(IP_U8_TYPE,    (IpIndex){IP_INDEX_U8_TYPE});
  ok &= ip_index_eq(IP_I32_TYPE,   (IpIndex){IP_INDEX_I32_TYPE});
  ok &= ip_index_eq(IP_USIZE_TYPE, (IpIndex){IP_INDEX_USIZE_TYPE});
  ok &= ip_index_eq(IP_VOID_TYPE,  (IpIndex){IP_INDEX_VOID_TYPE});

  // Tags correctly assigned at init.
  ok &= ip_tag(&pool, IP_BOOL_TYPE)  == IP_TAG_PRIMITIVE_TYPE;
  ok &= ip_tag(&pool, IP_VOID_TYPE)  == IP_TAG_PRIMITIVE_TYPE;
  ok &= ip_tag(&pool, IP_BOOL_TRUE)  == IP_TAG_RESERVED_VALUE;
  ok &= ip_tag(&pool, IP_BOOL_FALSE) == IP_TAG_RESERVED_VALUE;
  ok &= ip_tag(&pool, IP_VOID_VALUE) == IP_TAG_RESERVED_VALUE;

  // items_count equals the reserved-count sentinel.
  ok &= (pool.items_count == IP_RESERVED_COUNT);

  finish(ok);
  ip_free(&pool);
}

static void test_reserved_fast_path(void) {
  start("ip_get on a primitive uses the fast path (no new item)");
  InternPool pool;
  ip_init(&pool);
  size_t before = pool.items_count;
  size_t buckets_before = pool.bucket_used;

  IpKey k = {.kind = IPK_PRIMITIVE_TYPE, .primitive_type = IP_INDEX_I32_TYPE};
  IpIndex idx = ip_get(&pool, k);

  bool ok = ip_index_eq(idx, IP_I32_TYPE);
  ok &= (pool.items_count == before);
  ok &= (pool.bucket_used == buckets_before);

  finish(ok);
  ip_free(&pool);
}

static void test_round_trip(void) {
  start("round-trip: ip_key(ip_get(k)) reproduces k for a ptr_type");
  InternPool pool;
  ip_init(&pool);

  IpKey k = {.kind = IPK_PTR_TYPE,
             .ptr_type = {.elem = IP_I32_TYPE, .is_const = false}};
  IpIndex idx = ip_get(&pool, k);
  IpKey k2 = ip_key(&pool, idx);

  bool ok = (k2.kind == IPK_PTR_TYPE);
  ok &= ip_index_eq(k2.ptr_type.elem, IP_I32_TYPE);
  ok &= (k2.ptr_type.is_const == false);

  finish(ok);
  ip_free(&pool);
}

static void test_dedup_same_key(void) {
  start("dedup: two ip_get calls with equal keys return same IpIndex");
  InternPool pool;
  ip_init(&pool);

  // Use a key that's NOT one of the reserved compounds (^const u8 is
  // IP_C_STRING_TYPE) so this test exercises the bucket-probe dedup
  // path rather than the reserved-compound fast path.
  IpKey k = {.kind = IPK_PTR_TYPE,
             .ptr_type = {.elem = IP_I32_TYPE, .is_const = true}};
  IpIndex a = ip_get(&pool, k);
  IpIndex b = ip_get(&pool, k);

  bool ok = ip_index_eq(a, b);
  // Should have added exactly one item.
  ok &= (pool.items_count == IP_RESERVED_COUNT + 1);

  finish(ok);
  ip_free(&pool);
}

static void test_distinct_across_kinds(void) {
  start("distinct: ptr_type(T) and slice_type(T) get different IpIndex");
  InternPool pool;
  ip_init(&pool);

  IpKey kp = {.kind = IPK_PTR_TYPE,
              .ptr_type = {.elem = IP_I32_TYPE, .is_const = false}};
  IpKey ks = {.kind = IPK_SLICE_TYPE,
              .slice_type = {.elem = IP_I32_TYPE, .is_const = false}};
  IpIndex pi = ip_get(&pool, kp);
  IpIndex si = ip_get(&pool, ks);

  bool ok = !ip_index_eq(pi, si);

  finish(ok);
  ip_free(&pool);
}

static void test_const_discrimination(void) {
  start("ptr_type discriminates is_const");
  InternPool pool;
  ip_init(&pool);

  IpKey mut = {.kind = IPK_PTR_TYPE,
               .ptr_type = {.elem = IP_I32_TYPE, .is_const = false}};
  IpKey cst = {.kind = IPK_PTR_TYPE,
               .ptr_type = {.elem = IP_I32_TYPE, .is_const = true}};
  IpIndex mi = ip_get(&pool, mut);
  IpIndex ci = ip_get(&pool, cst);

  bool ok = !ip_index_eq(mi, ci);
  ok &= ip_index_eq(ip_get(&pool, mut), mi);  // re-dedup ok
  ok &= ip_index_eq(ip_get(&pool, cst), ci);

  finish(ok);
  ip_free(&pool);
}

static void test_array_size_discrim(void) {
  start("array_type discriminates on size");
  InternPool pool;
  ip_init(&pool);

  IpKey a16 = {.kind = IPK_ARRAY_TYPE,
               .array_type = {.elem = IP_U8_TYPE, .size = 16}};
  IpKey a32 = {.kind = IPK_ARRAY_TYPE,
               .array_type = {.elem = IP_U8_TYPE, .size = 32}};
  IpIndex i16 = ip_get(&pool, a16);
  IpIndex i32 = ip_get(&pool, a32);

  bool ok = !ip_index_eq(i16, i32);
  // Round-trip the size.
  IpKey r = ip_key(&pool, i32);
  ok &= (r.array_type.size == 32);

  finish(ok);
  ip_free(&pool);
}

static void test_fn_type_dedup(void) {
  start("fn_type dedup handles variable-length params");
  InternPool pool;
  ip_init(&pool);

  IpIndex params[2] = {IP_I32_TYPE, IP_U8_TYPE};
  IpKey k = {.kind = IPK_FN_TYPE};
  k.fn_type.ret = IP_I32_TYPE;
  k.fn_type.modifiers = 0;
  k.fn_type.params = params;
  k.fn_type.n_params = 2;

  IpIndex a = ip_get(&pool, k);

  // Build an equivalent key with a different params array, same contents.
  IpIndex params2[2] = {IP_I32_TYPE, IP_U8_TYPE};
  IpKey k2 = {.kind = IPK_FN_TYPE};
  k2.fn_type.ret = IP_I32_TYPE;
  k2.fn_type.modifiers = 0;
  k2.fn_type.params = params2;
  k2.fn_type.n_params = 2;
  IpIndex b = ip_get(&pool, k2);

  bool ok = ip_index_eq(a, b);

  // Different param order is a distinct fn type.
  IpIndex params3[2] = {IP_U8_TYPE, IP_I32_TYPE};
  IpKey k3 = k;
  k3.fn_type.params = params3;
  IpIndex c = ip_get(&pool, k3);
  ok &= !ip_index_eq(a, c);

  // Round-trip preserves all fields.
  IpKey r = ip_key(&pool, a);
  ok &= (r.kind == IPK_FN_TYPE);
  ok &= ip_index_eq(r.fn_type.ret, IP_I32_TYPE);
  ok &= (r.fn_type.n_params == 2);
  ok &= ip_index_eq(r.fn_type.params[0], IP_I32_TYPE);
  ok &= ip_index_eq(r.fn_type.params[1], IP_U8_TYPE);

  finish(ok);
  ip_free(&pool);
}

static void test_int_value_dedup(void) {
  start("int_value dedup on (type, value) pair");
  InternPool pool;
  ip_init(&pool);

  IpKey k = {.kind = IPK_INT_VALUE};
  k.int_value.type = IP_I32_TYPE;
  k.int_value.value = 42;
  IpIndex a = ip_get(&pool, k);
  IpIndex b = ip_get(&pool, k);

  bool ok = ip_index_eq(a, b);

  // Different value → different index.
  IpKey k2 = k;
  k2.int_value.value = 43;
  IpIndex c = ip_get(&pool, k2);
  ok &= !ip_index_eq(a, c);

  // Different type but same value → different index.
  IpKey k3 = k;
  k3.int_value.type = IP_U8_TYPE;
  IpIndex d = ip_get(&pool, k3);
  ok &= !ip_index_eq(a, d);

  // Round-trip.
  IpKey r = ip_key(&pool, a);
  ok &= (r.int_value.value == 42);
  ok &= ip_index_eq(r.int_value.type, IP_I32_TYPE);

  finish(ok);
  ip_free(&pool);
}

static void test_remove_then_reget(void) {
  start("ip_remove + subsequent ip_get of same key allocates fresh slot");
  InternPool pool;
  ip_init(&pool);

  IpKey k = {.kind = IPK_OPTIONAL_TYPE,
             .optional_type = {.elem = IP_U8_TYPE}};
  IpIndex a = ip_get(&pool, k);
  ip_remove(&pool, a);

  bool ok = (ip_tag(&pool, a) == IP_TAG_REMOVED);

  // Re-getting should produce a fresh index that's NOT the removed one.
  IpIndex b = ip_get(&pool, k);
  ok &= !ip_index_eq(a, b);
  ok &= (ip_tag(&pool, b) == IP_TAG_OPTIONAL_TYPE);

  finish(ok);
  ip_free(&pool);
}

static void test_struct_inline_identity(void) {
  start("struct type: inline nominal identity (zir), stable + deduped");
  InternPool pool;
  ip_init(&pool);

  // D2.1b: a struct type carries ONLY its nominal identity (zir_node_id),
  // inline-encoded. No fields in the pool — they live in the db field pool.
  uint32_t zir = 12345;
  IpIndex a = ip_get(&pool, ((IpKey){.kind = IPK_STRUCT_TYPE,
                                     .struct_type = {.zir_node_id = zir}}));
  bool ok = ip_index_is_valid(a);
  ok &= (ip_tag(&pool, a) == IP_TAG_STRUCT_TYPE);

  // Re-get the same zir → SAME index (stable dedup — the whole point: a
  // recompute reuses the index instead of minting a fresh one).
  IpIndex a2 = ip_get(&pool, ((IpKey){.kind = IPK_STRUCT_TYPE,
                                      .struct_type = {.zir_node_id = zir}}));
  ok &= ip_index_eq(a2, a);

  // A different zir → a different index (distinct nominal identities).
  IpIndex b = ip_get(&pool, ((IpKey){.kind = IPK_STRUCT_TYPE,
                                     .struct_type = {.zir_node_id = zir + 1}}));
  ok &= !ip_index_eq(b, a);

  // ip_key round-trips the zir; no field arrays are stored here.
  IpKey r = ip_key(&pool, a);
  ok &= (r.kind == IPK_STRUCT_TYPE && r.struct_type.zir_node_id == zir);

  // Self-reference still works: `^Node` is a plain ptr to the (already
  // stable) struct index — no wip dance needed.
  IpIndex ptr_to_self =
      ip_get(&pool, ((IpKey){.kind = IPK_PTR_TYPE,
                             .ptr_type = {.elem = a, .is_const = false}}));
  ok &= (ip_tag(&pool, ptr_to_self) == IP_TAG_PTR_TYPE);
  ok &= ip_index_eq(ip_key(&pool, ptr_to_self).ptr_type.elem, a);

  finish(ok);
  ip_free(&pool);
}

static void test_enum_inline_identity(void) {
  start("enum type: inline nominal identity (zir), stable + deduped");
  InternPool pool;
  ip_init(&pool);

  uint32_t zir = 777;
  IpIndex a = ip_get(&pool, ((IpKey){.kind = IPK_ENUM_TYPE,
                                     .enum_type = {.zir_node_id = zir}}));
  bool ok = ip_index_is_valid(a) && ip_tag(&pool, a) == IP_TAG_ENUM_TYPE;
  ok &= ip_index_eq(ip_get(&pool, ((IpKey){.kind = IPK_ENUM_TYPE,
                                           .enum_type = {.zir_node_id = zir}})),
                    a);
  ok &= !ip_index_eq(ip_get(&pool, ((IpKey){.kind = IPK_ENUM_TYPE,
                                            .enum_type = {.zir_node_id = zir + 1}})),
                     a);
  IpKey r = ip_key(&pool, a);
  ok &= (r.kind == IPK_ENUM_TYPE && r.enum_type.zir_node_id == zir);

  finish(ok);
  ip_free(&pool);
}

// =====================================================================
// T13 — borrowed pointers from ip_key stay valid across pool mutations.
//
// The old design's "valid until next ip_get" contract is replaced with
// "valid until ip_free" — extra_arena chunks never move. This test
// holds an IpKey from an fn type, then pumps thousands of subsequent
// ip_gets to force every realloc path (items array, bucket table, and
// chained-arena chunk growth). The held pointer must still read back
// the original param sequence.
// =====================================================================
static void test_arena_stability(void) {
  start("borrowed IpKey pointers remain valid across many ip_get calls");
  InternPool pool;
  ip_init(&pool);

  // Allocate an fn type with a recognizable param sequence, then
  // capture its IpKey (borrowed pointer into extra_arena).
  IpIndex params[4] = {IP_U8_TYPE, IP_I32_TYPE, IP_F64_TYPE, IP_USIZE_TYPE};
  IpKey fn_key = {.kind = IPK_FN_TYPE};
  fn_key.fn_type.ret = IP_I32_TYPE;
  fn_key.fn_type.modifiers = 0xCAFEBABE;
  fn_key.fn_type.params = params;
  fn_key.fn_type.n_params = 4;

  IpIndex fn_idx = ip_get(&pool, fn_key);
  IpKey held = ip_key(&pool, fn_idx);
  const IpIndex *held_params = held.fn_type.params;
  uint32_t held_mod = held.fn_type.modifiers;

  // Force many subsequent allocations: each iteration interns a unique
  // fn type with varying-length params, forcing extra_arena chunk
  // growth, items_* realloc, and buckets_grow events.
  for (uint32_t i = 0; i < 4096; i++) {
    IpIndex p[8];
    size_t n = (i % 7) + 1;
    for (size_t k = 0; k < n; k++) p[k] = (IpIndex){(i + (uint32_t)k) % 16};
    IpKey k = {.kind = IPK_FN_TYPE};
    k.fn_type.ret = IP_VOID_TYPE;
    k.fn_type.modifiers = i;
    k.fn_type.params = p;
    k.fn_type.n_params = n;
    (void)ip_get(&pool, k);
  }

  // The held pointer must still resolve correctly. If extra_arena
  // chunks had moved, this would dereference freed memory.
  bool ok = (held_mod == 0xCAFEBABE);
  ok &= ip_index_eq(held_params[0], IP_U8_TYPE);
  ok &= ip_index_eq(held_params[1], IP_I32_TYPE);
  ok &= ip_index_eq(held_params[2], IP_F64_TYPE);
  ok &= ip_index_eq(held_params[3], IP_USIZE_TYPE);

  // Cross-check: a fresh ip_key on the same IpIndex returns the same
  // pointer (it lives in the same arena slot).
  IpKey fresh = ip_key(&pool, fn_idx);
  ok &= (fresh.fn_type.params == held_params);

  finish(ok);
  ip_free(&pool);
}

// =====================================================================
// T14 — Effect-row dedup canonicalization + empty-row fast path.
// =====================================================================
static void test_effect_row(void) {
  start("effect_row dedup + empty row reserved");
  InternPool pool;
  ip_init(&pool);

  // Empty row hits the reserved IP_EMPTY_EFFECT_ROW.
  IpKey empty = {.kind = IPK_EFFECT_ROW};
  empty.effect_row.labels = NULL;
  empty.effect_row.n_labels = 0;
  empty.effect_row.tail = IP_EMPTY_EFFECT_ROW;
  IpIndex empty_idx = ip_get(&pool, empty);
  bool ok = ip_index_eq(empty_idx, IP_EMPTY_EFFECT_ROW);

  // Two distinct source arrays with the same sorted contents dedup.
  DefId a[3] = {{7}, {11}, {13}};
  DefId b[3] = {{7}, {11}, {13}};
  IpKey ka = {.kind = IPK_EFFECT_ROW};
  ka.effect_row.labels = a;
  ka.effect_row.n_labels = 3;
  IpKey kb = {.kind = IPK_EFFECT_ROW};
  kb.effect_row.labels = b;
  kb.effect_row.n_labels = 3;
  IpIndex ia = ip_get(&pool, ka);
  IpIndex ib = ip_get(&pool, kb);
  ok &= ip_index_eq(ia, ib);

  // Different contents → different IpIndex.
  DefId c[3] = {{7}, {11}, {14}};
  IpKey kc = ka;
  kc.effect_row.labels = c;
  IpIndex ic = ip_get(&pool, kc);
  ok &= !ip_index_eq(ia, ic);

  // Round-trip preserves the effect sequence.
  IpKey r = ip_key(&pool, ia);
  ok &= (r.effect_row.n_labels == 3);
  ok &= (r.effect_row.labels[0].idx == 7);
  ok &= (r.effect_row.labels[1].idx == 11);
  ok &= (r.effect_row.labels[2].idx == 13);

  finish(ok);
  ip_free(&pool);
}

// =====================================================================
// T15 — Reserved compound fast-path identity.
//
// ip_get for the four reserved compound types returns the pre-baked
// IpIndex without allocating new items. This is the fast-path that
// makes "is this a C string?" a single u32 compare.
// =====================================================================
static void test_reserved_compounds(void) {
  start("reserved compounds return the pre-baked IpIndex");
  InternPool pool;
  ip_init(&pool);

  size_t items_before = pool.items_count;

  // ^const u8 → IP_C_STRING_TYPE
  IpKey c_str = {.kind = IPK_PTR_TYPE,
                 .ptr_type = {.elem = IP_U8_TYPE, .is_const = true}};
  bool ok = ip_index_eq(ip_get(&pool, c_str), IP_C_STRING_TYPE);

  // ^void → IP_OPAQUE_PTR_TYPE
  IpKey opaque = {.kind = IPK_PTR_TYPE,
                  .ptr_type = {.elem = IP_VOID_TYPE, .is_const = false}};
  ok &= ip_index_eq(ip_get(&pool, opaque), IP_OPAQUE_PTR_TYPE);

  // ^const void → IP_CONST_OPAQUE_PTR_TYPE
  IpKey const_opaque = {.kind = IPK_PTR_TYPE,
                        .ptr_type = {.elem = IP_VOID_TYPE, .is_const = true}};
  ok &= ip_index_eq(ip_get(&pool, const_opaque), IP_CONST_OPAQUE_PTR_TYPE);

  // []const u8 → IP_STRING_SLICE_TYPE
  IpKey str_slice = {.kind = IPK_SLICE_TYPE,
                     .slice_type = {.elem = IP_U8_TYPE, .is_const = true}};
  ok &= ip_index_eq(ip_get(&pool, str_slice), IP_STRING_SLICE_TYPE);

  // No new items allocated — every ip_get hit a reserved slot.
  ok &= (pool.items_count == items_before);

  // Round-trip via ip_key: tags are recovered, payloads are inline
  // (no arena fetch).
  IpKey r = ip_key(&pool, IP_C_STRING_TYPE);
  ok &= (r.kind == IPK_PTR_TYPE);
  ok &= ip_index_eq(r.ptr_type.elem, IP_U8_TYPE);
  ok &= (r.ptr_type.is_const == true);

  finish(ok);
  ip_free(&pool);
}

// (T17 removed with the ip_wip_fn_* API in the D2 audit. Self-referential
// structural fn types now intern via a single structural ip_get once
// build_fn_type has resolved ret + params through the type cell type_of_def
// publishes — exercised at the query layer by the cycle tests in
// type_of_def_test, not via a pool-level wip primitive.)

// =====================================================================
// T18 — ip_init_with with custom sizing.
// =====================================================================
static void test_init_with(void) {
  start("ip_init_with: custom sizing populates reserved + handles workload");
  InternPool pool;
  ip_init_with(&pool, /*initial_buckets=*/1024, /*extra_chunk_size=*/65536);

  // Reserved slots still in place.
  bool ok = ip_index_eq(IP_BOOL_TYPE,    (IpIndex){IP_INDEX_BOOL_TYPE});
  ok &= ip_index_eq(IP_C_STRING_TYPE,    (IpIndex){IP_INDEX_C_STRING_TYPE});
  ok &= (pool.bucket_count == 1024);
  ok &= (pool.extra_arena.default_chunk_capacity == 65536);

  // Workload still works.
  IpKey k = {.kind = IPK_PTR_TYPE,
             .ptr_type = {.elem = IP_I32_TYPE, .is_const = false}};
  IpIndex a = ip_get(&pool, k);
  IpIndex b = ip_get(&pool, k);
  ok &= ip_index_eq(a, b);

  finish(ok);
  ip_free(&pool);
}

// =====================================================================
// T19 — ip_clear returns the pool to a fresh-init state.
// =====================================================================
static void test_clear(void) {
  start("ip_clear: user items gone, reserved items intact");
  InternPool pool;
  ip_init(&pool);

  // Allocate some user items.
  for (uint32_t i = 0; i < 100; i++) {
    IpKey k = {.kind = IPK_ARRAY_TYPE,
               .array_type = {.elem = IP_U8_TYPE, .size = (uint64_t)i + 1}};
    (void)ip_get(&pool, k);
  }
  size_t after_workload = pool.items_count;
  bool ok = (after_workload > IP_RESERVED_COUNT);

  // Clear.
  ip_clear(&pool);
  ok &= (pool.items_count == IP_RESERVED_COUNT);

  // Reserved slots still present and addressable.
  ok &= (ip_tag(&pool, IP_BOOL_TYPE) == IP_TAG_PRIMITIVE_TYPE);
  ok &= (ip_tag(&pool, IP_C_STRING_TYPE) == IP_TAG_PTR_CONST_TYPE);

  // Reserved-compound bucket registration survived clear:
  // ip_get(^const u8) still returns IP_C_STRING_TYPE.
  IpKey c_str = {.kind = IPK_PTR_TYPE,
                 .ptr_type = {.elem = IP_U8_TYPE, .is_const = true}};
  ok &= ip_index_eq(ip_get(&pool, c_str), IP_C_STRING_TYPE);

  // After clear, a fresh user intern lands at IP_RESERVED_COUNT.
  IpKey ptr_i32 = {.kind = IPK_PTR_TYPE,
                   .ptr_type = {.elem = IP_I32_TYPE, .is_const = false}};
  IpIndex i = ip_get(&pool, ptr_i32);
  ok &= (i.v == IP_RESERVED_COUNT);

  finish(ok);
  ip_free(&pool);
}

// =====================================================================
// T20 — ip_format renders common types as expected.
// =====================================================================
static void test_format(void) {
  start("ip_format: human-readable type strings");
  InternPool pool;
  ip_init(&pool);

  char buf[128];
  bool ok = true;

  // Primitives.
  ip_format(&pool, IP_BOOL_TYPE, buf, sizeof(buf));
  ok &= (strcmp(buf, "bool") == 0);
  ip_format(&pool, IP_I32_TYPE, buf, sizeof(buf));
  ok &= (strcmp(buf, "i32") == 0);

  // Reserved values.
  ip_format(&pool, IP_BOOL_TRUE, buf, sizeof(buf));
  ok &= (strcmp(buf, "true") == 0);
  ip_format(&pool, IP_ZERO_USIZE, buf, sizeof(buf));
  ok &= (strcmp(buf, "0_usize") == 0);

  // Reserved compounds.
  ip_format(&pool, IP_C_STRING_TYPE, buf, sizeof(buf));
  ok &= (strcmp(buf, "^const u8") == 0);
  ip_format(&pool, IP_STRING_SLICE_TYPE, buf, sizeof(buf));
  ok &= (strcmp(buf, "[]const u8") == 0);
  ip_format(&pool, IP_OPAQUE_PTR_TYPE, buf, sizeof(buf));
  ok &= (strcmp(buf, "^void") == 0);

  // User compound: []i32
  IpKey slice_i32 = {.kind = IPK_SLICE_TYPE,
                     .slice_type = {.elem = IP_I32_TYPE, .is_const = false}};
  ip_format(&pool, ip_get(&pool, slice_i32), buf, sizeof(buf));
  ok &= (strcmp(buf, "[]i32") == 0);

  // [16]u8
  IpKey arr = {.kind = IPK_ARRAY_TYPE,
               .array_type = {.elem = IP_U8_TYPE, .size = 16}};
  ip_format(&pool, ip_get(&pool, arr), buf, sizeof(buf));
  ok &= (strcmp(buf, "[16]u8") == 0);

  // fn(i32, u8) -> i32
  IpIndex params[2] = {IP_I32_TYPE, IP_U8_TYPE};
  IpKey fk = {.kind = IPK_FN_TYPE};
  fk.fn_type.ret = IP_I32_TYPE;
  fk.fn_type.modifiers = 0;
  fk.fn_type.params = params;
  fk.fn_type.n_params = 2;
  ip_format(&pool, ip_get(&pool, fk), buf, sizeof(buf));
  ok &= (strcmp(buf, "fn(i32, u8) -> i32") == 0);

  // Effect row <def#3, def#7>.
  DefId effects[2] = {{3}, {7}};
  IpKey er = {.kind = IPK_EFFECT_ROW};
  er.effect_row.labels = effects;
  er.effect_row.n_labels = 2;
  er.effect_row.tail = IP_EMPTY_EFFECT_ROW;
  ip_format(&pool, ip_get(&pool, er), buf, sizeof(buf));
  ok &= (strcmp(buf, "<def#3, def#7>") == 0);

  // Empty effect row.
  ip_format(&pool, IP_EMPTY_EFFECT_ROW, buf, sizeof(buf));
  ok &= (strcmp(buf, "<>") == 0);

  // Truncation: small buffer keeps NUL-termination and returns the
  // would-have-been count.
  size_t need = ip_format(&pool, IP_C_STRING_TYPE, buf, 4);
  ok &= (need == 9);  // "^const u8" = 9 chars
  ok &= (buf[3] == '\0');  // NUL within the 4-byte buffer

  finish(ok);
  ip_free(&pool);
}

// =====================================================================

int main(void) {
  printf("intern_pool unit tests\n");

  test_reserved_indices();
  test_reserved_fast_path();
  test_round_trip();
  test_dedup_same_key();
  test_distinct_across_kinds();
  test_const_discrimination();
  test_array_size_discrim();
  test_fn_type_dedup();
  test_int_value_dedup();
  test_remove_then_reget();
  test_struct_inline_identity();
  test_enum_inline_identity();
  test_arena_stability();
  test_effect_row();
  test_reserved_compounds();
  test_init_with();
  test_clear();
  test_format();

  printf("\n%d pass, %d fail\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
