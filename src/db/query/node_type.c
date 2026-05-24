#include "node_type.h"

#include "../../sema/sema.h"
#include "../db.h"
#include "fn_signature.h"
#include "infer_body.h"
#include "type_of_def.h"

// Unified node→type router. See node_type.h for the architectural
// notes. This function is NOT itself salsa-tracked (no slot, no
// fingerprint) — it's a pure dispatch over already-salsa-tracked
// sub-queries. Each sub-query call short-cuts on a cache hit, so the
// router's hot-path cost is O(parent_depth) for the def lookup + a
// constant number of pool dereferences for the range lookups.
IpIndex db_query_node_type(struct db *s, FileId fid, AstNodeId node) {
  if (!file_id_valid(fid) || node.idx == AST_NODE_ID_NONE.idx)
    return IP_NONE;

  // 1. Find the enclosing top-level def. The parent-walk inside
  //    db_get_def_for_node drives db_query_node_to_def to ensure the
  //    file's reverse-index is current, so this also registers the
  //    right salsa dep for any caller running inside a query frame.
  DefId def = db_get_def_for_node(s, fid, node);
  if (def.idx == DEF_ID_NONE.idx)
    return IP_NONE;

  // 2. Dispatch by the enclosing def's kind to the per-decl range that
  //    owns this node's type. Each branch drives the matching query
  //    first (salsa cache hit on warm runs), then reads the range from
  //    the per-kind column.
  DefKind kind = db_def_kind(s, def);
  switch (kind) {
  case KIND_FUNCTION: {
    // Drive both fn-signature and infer-body so the ranges are stamped.
    (void)db_query_fn_signature(s, def);
    (void)db_query_infer_body(s, def);

    uint32_t row = db_def_row(s, def, KIND_FUNCTION);
    // Try body range first — it's the larger range and the more common
    // hit site for IDE hover. Then signature range. Both ranges are
    // disjoint by construction (body covers the lambda's body subtree,
    // signature covers params + ret-type) so order is just a perf
    // optimization, not correctness.
    NodeTypesRange body_range =
        *(NodeTypesRange *)vec_get(&s->fns.body_node_types, row);
    IpIndex t = sema_node_types_range_lookup(s, body_range, node);
    if (t.v != IP_NONE.v)
      return t;
    NodeTypesRange sig_range =
        *(NodeTypesRange *)vec_get(&s->fns.signature_node_types, row);
    t = sema_node_types_range_lookup(s, sig_range, node);
    if (t.v != IP_NONE.v)
      return t;
    // The node IS the fn's own decl (top-level identifier outside the
    // body/sig subtrees). Return the fn type directly.
    return db_query_type_of_def(s, def);
  }
  case KIND_STRUCT:
  case KIND_UNION: {
    (void)db_query_type_of_def(s, def); // populates field_node_types
    uint32_t row = db_def_row(s, def, kind);
    NodeTypesRange field_range =
        *(NodeTypesRange *)vec_get(&s->structs.field_node_types, row);
    IpIndex t = sema_node_types_range_lookup(s, field_range, node);
    if (t.v != IP_NONE.v)
      return t;
    // Falls through: the node IS the struct's own decl identifier.
    return db_query_type_of_def(s, def);
  }
  default:
    // KIND_CONSTANT / KIND_VARIABLE / KIND_ENUM / KIND_EFFECT /
    // KIND_HANDLER / KIND_NONE — for now, return the decl's overall
    // type. The value-expression subtree of a CONST/VAR is uncovered
    // by any per-decl range today; lookups against sub-nodes return
    // IP_NONE. That's the "future work" noted in the plan (consider
    // extending type_of_def to also build a value-expr range, or
    // adding a dedicated db_query_const_value_types).
    return db_query_type_of_def(s, def);
  }
}
