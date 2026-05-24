#include "compact.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "db.h"
#include "storage/vec.h"

static uint64_t compact_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// =============================================================================
// Mark-and-copy compaction for the shared salsa pools.
//
// Each pool grows monotonically as queries re-run. When a query body
// finishes, it writes a fresh (off, len) range into its owning column;
// the previous range stays in the pool, unreachable. This file's
// compactors reclaim those dead entries by:
//
//   1. MARK   — walk every column entry, collect (cell_ptr, old_off, len).
//   2. PLAN   — sort by old_off (stable so duplicates dedupe correctly),
//               assign new_off as a running prefix sum.
//   3. COPY   — allocate a fresh Vec, copy each live range into it.
//   4. REWRITE — walk cells again, write each new_off back via cell_ptr.
//   5. SWAP   — vec_free the old pool; replace it with the new one.
//
// Trigger is at db_request_end (db_pools_maybe_compact). Safety: every
// query body has returned by that point, so no Vec.data raw pointer
// dereferenced by a caller's frame survives across the relocation.
// =============================================================================

// Shared remap record — used by every compactor. Pool-specific
// rewrites use the cell back-pointer to update offsets in-place.
typedef struct {
  uint32_t old_off;
  uint32_t new_off;
  uint32_t len;
  void *cell; // pointer to the owning column entry (per-pool type)
} RangeRemap;

static int cmp_remap_by_old_off(const void *a, const void *b) {
  const RangeRemap *ra = (const RangeRemap *)a;
  const RangeRemap *rb = (const RangeRemap *)b;
  if (ra->old_off < rb->old_off)
    return -1;
  if (ra->old_off > rb->old_off)
    return 1;
  return 0;
}

// =============================================================================
// Pool 1: db.node_types_pool — IpIndex slots referenced by NodeTypesRange
// entries on db.fns.body_node_types, db.fns.signature_node_types,
// db.structs.field_node_types.
//
// Sentinel: offset 0 holds a single IP_NONE slot that the empty-range
// fast path relies on. Compaction pins it as the new pool's offset 0.
// =============================================================================

// Collect every live NodeTypesRange from a per-kind column into `out`.
// Skips empty ranges (types_len == 0) — they don't occupy pool space
// and don't need remapping (their offset is the sentinel).
static void collect_node_types_ranges(Vec *column, Vec *out) {
  for (size_t i = 0; i < column->count; i++) {
    NodeTypesRange *r = (NodeTypesRange *)vec_get(column, i);
    if (r->types_len == 0)
      continue;
    RangeRemap rm = {
        .old_off = r->types_off,
        .new_off = 0, // filled in PLAN phase
        .len = r->types_len,
        .cell = r,
    };
    vec_push(out, &rm);
  }
}

void db_compact_node_types_pool(struct db *s) {
  uint64_t t0 = compact_now_ns();
  uint32_t pre_count = (uint32_t)s->node_types_pool.count;

  // MARK: collect live ranges from every per-kind column that owns one.
  Vec ranges;
  vec_init(&ranges, sizeof(RangeRemap));
  collect_node_types_ranges(&s->fns.body_node_types, &ranges);
  collect_node_types_ranges(&s->fns.signature_node_types, &ranges);
  collect_node_types_ranges(&s->constants.value_node_types, &ranges);
  collect_node_types_ranges(&s->variables.value_node_types, &ranges);
  collect_node_types_ranges(&s->structs.field_node_types, &ranges);

  // PLAN: sort by old_off, assign new_off as prefix sum starting AFTER
  // the pinned sentinel slot at new pool offset 0.
  if (ranges.count > 0)
    qsort(ranges.data, ranges.count, sizeof(RangeRemap), cmp_remap_by_old_off);
  uint32_t new_off_cursor = 1; // 1 = first slot after the sentinel
  for (size_t i = 0; i < ranges.count; i++) {
    RangeRemap *rm = (RangeRemap *)vec_get(&ranges, i);
    rm->new_off = new_off_cursor;
    new_off_cursor += rm->len;
  }

  // COPY: allocate new pool, copy sentinel + live ranges.
  Vec new_pool;
  vec_init(&new_pool, sizeof(IpIndex));
  vec_reserve(&new_pool, new_off_cursor);
  // Sentinel slot at offset 0.
  IpIndex none = IP_NONE;
  vec_push(&new_pool, &none);
  for (size_t i = 0; i < ranges.count; i++) {
    RangeRemap *rm = (RangeRemap *)vec_get(&ranges, i);
    IpIndex *src = (IpIndex *)vec_get(&s->node_types_pool, rm->old_off);
    for (uint32_t j = 0; j < rm->len; j++)
      vec_push(&new_pool, &src[j]);
  }

  // REWRITE: each cell's types_off → its new_off. Empty ranges keep
  // their existing zero / unused offsets (we never enter that branch
  // via the lookup function).
  for (size_t i = 0; i < ranges.count; i++) {
    RangeRemap *rm = (RangeRemap *)vec_get(&ranges, i);
    NodeTypesRange *cell = (NodeTypesRange *)rm->cell;
    cell->types_off = rm->new_off;
  }

  // SWAP: free old pool, install new pool.
  vec_free(&s->node_types_pool);
  s->node_types_pool = new_pool;

  vec_free(&ranges);

  // Update the trigger watermark.
  uint32_t post_count = (uint32_t)s->node_types_pool.count;
  s->last_compacted_node_types_count = post_count;

  // Stats: bytes reclaimed counts the SHRINKAGE (pre - post) × elem_size.
  s->compact_stats.n_compactions[0]++;
  s->compact_stats.bytes_reclaimed[0] +=
      (uint64_t)(pre_count - post_count) * sizeof(IpIndex);
  s->compact_stats.total_ns[0] += compact_now_ns() - t0;
}

// =============================================================================
// Pool 2: db.body_scope_rows + db.body_scope_binds + db.node_to_scope
// All three are driven by db.fns.body[*] — each FnBody has three (off,
// len) pairs into the three parallel pools. The three pools are
// compacted independently but in the same pass to amortize the FnBody
// walk.
// =============================================================================

// Direction-indexed iteration through the three parallel pool pairs
// of FnBody. Each entry pairs (pool, FnBody-offset-field-getter,
// FnBody-len-field-getter, element_size).
//
// To keep the rewrite straightforward we use direct offsetof / size
// constants. C doesn't have pointer-to-data-member, so we just
// duplicate the small loop for each of the three sub-pools.

static void collect_body_scope_ranges(Vec *fns_body, Vec *out_rows,
                                      Vec *out_binds, Vec *out_n2s) {
  for (size_t i = 0; i < fns_body->count; i++) {
    FnBody *fb = (FnBody *)vec_get(fns_body, i);
    if (fb->scope_len > 0) {
      RangeRemap rm = {
          .old_off = fb->scope_off,
          .new_off = 0,
          .len = fb->scope_len,
          .cell = fb,
      };
      vec_push(out_rows, &rm);
    }
    if (fb->bind_len > 0) {
      RangeRemap rm = {
          .old_off = fb->bind_off,
          .new_off = 0,
          .len = fb->bind_len,
          .cell = fb,
      };
      vec_push(out_binds, &rm);
    }
    if (fb->n2s_len > 0) {
      RangeRemap rm = {
          .old_off = fb->n2s_off,
          .new_off = 0,
          .len = fb->n2s_len,
          .cell = fb,
      };
      vec_push(out_n2s, &rm);
    }
  }
}

// Plan + copy + swap a single sub-pool. Element write is done by the
// caller because the FnBody field to update varies (scope_off /
// bind_off / n2s_off). `rewrite_field_offset_bytes` is the byte
// offset within FnBody of the field we update.
static void compact_one_subpool(Vec *old_pool, Vec *ranges, size_t elem_size,
                                size_t rewrite_field_offset_bytes) {
  if (ranges->count > 0)
    qsort(ranges->data, ranges->count, sizeof(RangeRemap),
          cmp_remap_by_old_off);
  uint32_t new_off_cursor = 0;
  for (size_t i = 0; i < ranges->count; i++) {
    RangeRemap *rm = (RangeRemap *)vec_get(ranges, i);
    rm->new_off = new_off_cursor;
    new_off_cursor += rm->len;
  }

  Vec new_pool;
  vec_init(&new_pool, elem_size);
  vec_reserve(&new_pool, new_off_cursor);
  for (size_t i = 0; i < ranges->count; i++) {
    RangeRemap *rm = (RangeRemap *)vec_get(ranges, i);
    void *src = (char *)old_pool->data + rm->old_off * elem_size;
    for (uint32_t j = 0; j < rm->len; j++) {
      vec_push(&new_pool, (char *)src + j * elem_size);
    }
  }

  for (size_t i = 0; i < ranges->count; i++) {
    RangeRemap *rm = (RangeRemap *)vec_get(ranges, i);
    uint32_t *field =
        (uint32_t *)((char *)rm->cell + rewrite_field_offset_bytes);
    *field = rm->new_off;
  }

  vec_free(old_pool);
  *old_pool = new_pool;
}

void db_compact_body_scope_pools(struct db *s) {
  uint64_t t0 = compact_now_ns();
  uint64_t pre_bytes =
      (uint64_t)s->body_scope_rows.count * sizeof(ScopeRow) +
      (uint64_t)s->body_scope_binds.count * sizeof(ScopedBind) +
      (uint64_t)s->node_to_scope.count * sizeof(uint32_t);

  Vec rows, binds, n2s;
  vec_init(&rows, sizeof(RangeRemap));
  vec_init(&binds, sizeof(RangeRemap));
  vec_init(&n2s, sizeof(RangeRemap));
  collect_body_scope_ranges(&s->fns.body, &rows, &binds, &n2s);

  compact_one_subpool(&s->body_scope_rows, &rows, sizeof(ScopeRow),
                      offsetof(FnBody, scope_off));
  compact_one_subpool(&s->body_scope_binds, &binds, sizeof(ScopedBind),
                      offsetof(FnBody, bind_off));
  compact_one_subpool(&s->node_to_scope, &n2s, sizeof(uint32_t),
                      offsetof(FnBody, n2s_off));

  vec_free(&rows);
  vec_free(&binds);
  vec_free(&n2s);

  s->last_compacted_body_scope_rows_count = (uint32_t)s->body_scope_rows.count;
  s->last_compacted_body_scope_binds_count =
      (uint32_t)s->body_scope_binds.count;
  s->last_compacted_node_to_scope_count = (uint32_t)s->node_to_scope.count;

  uint64_t post_bytes =
      (uint64_t)s->body_scope_rows.count * sizeof(ScopeRow) +
      (uint64_t)s->body_scope_binds.count * sizeof(ScopedBind) +
      (uint64_t)s->node_to_scope.count * sizeof(uint32_t);
  s->compact_stats.n_compactions[1]++;
  s->compact_stats.bytes_reclaimed[1] += (pre_bytes - post_bytes);
  s->compact_stats.total_ns[1] += compact_now_ns() - t0;
}

// =============================================================================
// Pool 3: db.scopes.decl_pool — keyed by (decl_lo[i], decl_len[i])
// MARK: only ScopeIds reachable from db.namespaces.exports[*] (plus the
//       synthetic primitives_scope) are live. Every other ScopeId is a
//       historical scope whose range can be reclaimed.
// COPY: visit live scopes in scope-id order (preserves owning_modules
//       layout for the rewrite phase); copy each one's slice to the
//       new pool; record the new lo.
// REWRITE: stamp the new (lo, len) onto each live scope's columns. Dead
//       scopes get their decl_len zeroed (slot stays in the column for
//       stable scope-id indexing — Zig-style free-list reuse is the
//       next optimization, out of scope here).
// =============================================================================

void db_compact_decl_pool(struct db *s) {
  uint64_t t0 = compact_now_ns();
  uint32_t pre_count = (uint32_t)s->scopes.decl_pool.count;
  size_t scope_count = s->scopes.parents.count;

  if (scope_count == 0) {
    s->last_compacted_decl_pool_count = (uint32_t)s->scopes.decl_pool.count;
    return;
  }

  // Mark: a parallel bitset over scope ids (1 byte per scope is
  // cheap at typical workspace scale; avoids a HashMap allocation).
  Vec live;
  vec_init(&live, sizeof(uint8_t));
  for (size_t i = 0; i < scope_count; i++)
    vec_push_zero(&live);

  if (s->primitives_scope.idx < scope_count)
    *(uint8_t *)vec_get(&live, s->primitives_scope.idx) = 1;
  for (size_t i = 0; i < s->namespaces.exports.count; i++) {
    NamespaceScopes *ns = (NamespaceScopes *)vec_get(&s->namespaces.exports, i);
    if (ns->internal.idx != SCOPE_ID_NONE.idx && ns->internal.idx < scope_count)
      *(uint8_t *)vec_get(&live, ns->internal.idx) = 1;
    if (ns->exported.idx != SCOPE_ID_NONE.idx && ns->exported.idx < scope_count)
      *(uint8_t *)vec_get(&live, ns->exported.idx) = 1;
  }

  // Copy live ranges into a fresh pool, in scope-id order. Stamp the
  // new (lo, len) immediately; dead scopes get len=0 (lo is don't-care
  // when len=0, but we zero it for tidiness).
  Vec new_pool;
  vec_init(&new_pool, sizeof(DeclEntry));
  for (size_t i = 0; i < scope_count; i++) {
    uint8_t is_live = *(uint8_t *)vec_get(&live, i);
    if (!is_live) {
      *(uint32_t *)vec_get(&s->scopes.decl_lo, i) = 0;
      *(uint32_t *)vec_get(&s->scopes.decl_len, i) = 0;
      continue;
    }
    uint32_t old_lo = *(uint32_t *)vec_get(&s->scopes.decl_lo, i);
    uint32_t len = *(uint32_t *)vec_get(&s->scopes.decl_len, i);
    uint32_t new_lo = (uint32_t)new_pool.count;
    for (uint32_t j = 0; j < len; j++) {
      DeclEntry *de = (DeclEntry *)vec_get(&s->scopes.decl_pool, old_lo + j);
      vec_push(&new_pool, de);
    }
    *(uint32_t *)vec_get(&s->scopes.decl_lo, i) = new_lo;
    // decl_len[i] is already correct (unchanged by the copy).
  }
  vec_free(&live);

  vec_free(&s->scopes.decl_pool);
  s->scopes.decl_pool = new_pool;

  uint32_t post_count = (uint32_t)s->scopes.decl_pool.count;
  s->last_compacted_decl_pool_count = post_count;
  s->compact_stats.n_compactions[2]++;
  s->compact_stats.bytes_reclaimed[2] +=
      (uint64_t)(pre_count - post_count) * sizeof(DeclEntry);
  s->compact_stats.total_ns[2] += compact_now_ns() - t0;
}

// =============================================================================
// Top-level dispatcher. Called from db_request_end.
// =============================================================================

void db_pools_maybe_compact(struct db *s) {
  uint32_t threshold = s->compact_min_threshold;
  if (s->node_types_pool.count > threshold &&
      s->node_types_pool.count >
          s->last_compacted_node_types_count * ORE_COMPACT_GROWTH_FACTOR) {
    db_compact_node_types_pool(s);
  }
  // Body-scope pools share a single trigger: if ANY of the three has
  // grown enough, compact all three together (they share the FnBody
  // walk). Use the rows pool as the canonical signal because it's
  // typically the smallest of the three, so growth there implies
  // growth elsewhere.
  if (s->body_scope_rows.count > threshold &&
      s->body_scope_rows.count >
          s->last_compacted_body_scope_rows_count * ORE_COMPACT_GROWTH_FACTOR) {
    db_compact_body_scope_pools(s);
  }
  if (s->scopes.decl_pool.count > threshold &&
      s->scopes.decl_pool.count >
          s->last_compacted_decl_pool_count * ORE_COMPACT_GROWTH_FACTOR) {
    db_compact_decl_pool(s);
  }
}
