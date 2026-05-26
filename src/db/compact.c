#include "compact.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../support/data_structure/vec.h"
#include "db.h"

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
//
// Phase 4 cleanup: the former node_types_pool is gone — per-decl
// NodeTypesRanges now own their own HashMaps, so there's no shared
// pool to compact. The former node_to_scope sub-pool is also gone for
// the same reason (per-fn HashMap<SyntaxNodePtr, ScopeId> in
// db.fns.scope_map). Only body_scope_rows + body_scope_binds + decl_pool
// remain.
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
// Pool: db.body_scope_rows + db.body_scope_binds
//
// Both are driven by db.fns.body[*] — each FnBody has two (off, len)
// pairs into the two parallel pools. They're compacted together to
// amortize the FnBody walk.
// =============================================================================

static void collect_body_scope_ranges(Vec *fns_body, Vec *out_rows,
                                      Vec *out_binds) {
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
  }
}

// Plan + copy + swap a single sub-pool. Element write is done by the
// caller because the FnBody field to update varies (scope_off /
// bind_off). `rewrite_field_offset_bytes` is the byte offset within
// FnBody of the field we update.
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
      (uint64_t)s->body_scope_binds.count * sizeof(ScopedBind);

  Vec rows, binds;
  vec_init(&rows, sizeof(RangeRemap));
  vec_init(&binds, sizeof(RangeRemap));
  collect_body_scope_ranges(&s->fns.body, &rows, &binds);

  compact_one_subpool(&s->body_scope_rows, &rows, sizeof(ScopeRow),
                      offsetof(FnBody, scope_off));
  compact_one_subpool(&s->body_scope_binds, &binds, sizeof(ScopedBind),
                      offsetof(FnBody, bind_off));

  vec_free(&rows);
  vec_free(&binds);

  s->last_compacted_body_scope_rows_count = (uint32_t)s->body_scope_rows.count;
  s->last_compacted_body_scope_binds_count =
      (uint32_t)s->body_scope_binds.count;

  uint64_t post_bytes =
      (uint64_t)s->body_scope_rows.count * sizeof(ScopeRow) +
      (uint64_t)s->body_scope_binds.count * sizeof(ScopedBind);
  s->compact_stats.n_compactions[0]++;
  s->compact_stats.bytes_reclaimed[0] += (pre_bytes - post_bytes);
  s->compact_stats.total_ns[0] += compact_now_ns() - t0;
}

// =============================================================================
// Pool: db.scopes.decl_pool — keyed by (decl_lo[i], decl_len[i])
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
  s->compact_stats.n_compactions[1]++;
  s->compact_stats.bytes_reclaimed[1] +=
      (uint64_t)(pre_count - post_count) * sizeof(DeclEntry);
  s->compact_stats.total_ns[1] += compact_now_ns() - t0;
}

// =============================================================================
// Top-level dispatcher. Called from db_request_end.
// =============================================================================

void db_pools_maybe_compact(struct db *s) {
  uint32_t threshold = s->compact_min_threshold;
  // Body-scope pools share a single trigger: if either has grown enough,
  // compact both together (they share the FnBody walk). Use the rows
  // pool as the canonical signal — typically the smaller, so growth
  // there implies growth elsewhere.
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
