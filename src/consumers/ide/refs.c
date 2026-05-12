#include "refs.h"

#include <stdlib.h>

#include "../../common/arena.h"
#include "../../common/hashmap.h"
#include "../../common/vec.h"
#include "../body/body_store.h" // id_to_expr (R8)
#include "../resolve/resolve.h"
#include "../sema.h"

struct CollectCtx {
  struct Sema *s;
  DefId target;
  uint64_t current_rev;
  Vec *out;
};

// resolve_ref_entries key encoding (post-R8):
//   ((uint64_t)decl.idx << 36) | ((uint64_t)local << 4) | (ns & 0xF)
// (see ids.h `expr_id_ns_key`). Reconstruct the ExprId, look up the
// current parse's Expr* via id_to_expr, and emit its NodeId.
//
// Staleness filter: a deleted Ident's slot stays in QUERY_DONE with
// its last-validated `def`, because nothing in the current revision
// reaches it to force revalidation. Such slots are *behind* on
// verified_rev — they were never confirmed live at the current
// revision. Filtering on `verified_rev == current_revision` excludes
// them, eliminating the ghost-reference problem the maintained
// index couldn't solve.
//
// This works because the AST-walking pass that drives every typecheck
// (sema_check_module → query_type_of_def → query_resolve_ref for every
// Ident in the AST) brings every live slot up to current_revision via
// sema_query_begin/sema_revalidate. Slots not touched by that pass
// belong to deleted nodes by definition.
static bool visit_entry(uint64_t key, void *val, void *ud) {
  struct CollectCtx *ctx = (struct CollectCtx *)ud;
  struct ResolveRefEntry *e = (struct ResolveRefEntry *)val;
  if (!e || e->query.state != QUERY_DONE)
    return true;
  if (e->query.verified_rev != ctx->current_rev)
    return true;
  if (e->def.idx != ctx->target.idx)
    return true;
  // Decode ExprId from the key. We emit NodeIds (current API);
  // callers can migrate to ExprIds in a follow-up.
  ExprId id = {
      .decl = {.idx = (uint32_t)(key >> 36)},
      .local = (uint32_t)((key >> 4) & 0xFFFFFFFFu),
  };
  struct Expr *expr = id_to_expr(ctx->s, id);
  if (!expr)
    return true;
  vec_push(ctx->out, &expr->id);
  return true;
}

static int compare_node_ids(const void *a, const void *b) {
  uint32_t ai = ((const struct NodeId *)a)->id;
  uint32_t bi = ((const struct NodeId *)b)->id;
  if (ai != bi)
    return ai < bi ? -1 : 1;
  return 0;
}

Vec *query_references_of(struct Sema *s, DefId def, Arena *out_arena) {
  if (!s || !def_id_is_valid(def) || !out_arena)
    return NULL;
  Vec *out = vec_new_in(out_arena, sizeof(struct NodeId));
  struct CollectCtx ctx = {
      .s = s, .target = def, .current_rev = s->current_revision, .out = out};
  hashmap_foreach(&s->resolve_ref_entries, visit_entry, &ctx);
  // Sort for deterministic output across runs — hashmap iteration
  // order is implementation-defined. Same rationale as
  // diag_collect_all.
  if (out->count > 1) {
    qsort(out->data, out->count, sizeof(struct NodeId), compare_node_ids);
  }
  return out;
}
