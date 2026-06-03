// =====================================================================
// Capability layer — wrappers that record salsa deps on raw column reads.
//
// See capability.h for design. Each `db_read_*` wrapper:
//   1. Asserts an active query frame (caller must be inside a query).
//   2. Touches the producing query (`db_query_*`) to ensure the column
//      is computed AND record the dep on the caller's frame.
//   3. Returns the raw column value.
//
// Raw column access inside this file is `// LINT_UNTRACKED_OK` by
// convention — this file is excluded from the lint gate entirely
// because it IS the gate.
// =====================================================================

#define ORE_ENGINE_PRIVATE
#include "capability.h"
#include "result_columns.h"

#include "../diag/ast_id.h"
#include "../db.h"
#include "../ids/ids.h"

#include <assert.h>

// External tracked-query entry points we delegate into.
extern IpIndex            db_query_type_of_def(db_query_ctx *ctx, DefId d);
extern const FnSignature *db_query_fn_signature(db_query_ctx *ctx, DefId d);
extern NodeTypesRange     db_query_infer_body(db_query_ctx *ctx, DefId d);
extern const FnBody      *db_query_body_scopes(db_query_ctx *ctx, DefId d);
extern struct GreenNode  *db_query_file_ast(db_query_ctx *ctx, FileId fid);

// =====================================================================
// Per-def reads — TYPE_OF_DECL is the universal anchor.
// =====================================================================
//
// TYPE_OF_DECL is allocated for every DefId (every kind). Touching it
// ensures the def's identity row is alive at the current revision AND
// records a per-def dep that catches reclamation / content change.

StrId db_read_def_name(db_query_ctx *ctx, DefId d) {
  struct db *s = (struct db *)ctx;
  assert(db_query_stack_top(s) != NULL &&
         "db_read_def_name: must be called from inside a query frame. "
         "Driver-level callers should use db_get_def_name_untracked.");
  if (d.idx == 0)
    return (StrId){0};
  (void)db_query_type_of_def(ctx, d);
  if (d.idx >= s->defs.names.count)
    return (StrId){0};
  return *(StrId *)vec_get(&s->defs.names, d.idx);
}

DefKind db_read_def_kind(db_query_ctx *ctx, DefId d) {
  struct db *s = (struct db *)ctx;
  assert(db_query_stack_top(s) != NULL &&
         "db_read_def_kind: must be called from inside a query frame. "
         "Driver-level callers should use db_get_def_kind_untracked.");
  if (d.idx == 0)
    return KIND_NONE;
  (void)db_query_type_of_def(ctx, d);
  if (d.idx >= s->defs.kinds.count)
    return KIND_NONE;
  return *(DefKind *)vec_get(&s->defs.kinds, d.idx);
}

NamespaceId db_read_def_parent_module(db_query_ctx *ctx, DefId d) {
  struct db *s = (struct db *)ctx;
  assert(db_query_stack_top(s) != NULL &&
         "db_read_def_parent_module: must be called from inside a query frame. "
         "Driver-level callers should use db_get_def_parent_module_untracked.");
  if (d.idx == 0)
    return (NamespaceId){0};
  (void)db_query_type_of_def(ctx, d);
  if (d.idx >= s->defs.parent_modules.count)
    return (NamespaceId){0};
  return *(NamespaceId *)vec_get(&s->defs.parent_modules, d.idx);
}

uint32_t db_read_def_kind_row(db_query_ctx *ctx, DefId d) {
  struct db *s = (struct db *)ctx;
  assert(db_query_stack_top(s) != NULL &&
         "db_read_def_kind_row: must be called from inside a query frame. "
         "Driver-level callers should use db_get_def_kind_row_untracked.");
  if (d.idx == 0)
    return 0;
  (void)db_query_type_of_def(ctx, d);
  if (d.idx >= s->defs.kind_row.count)
    return 0;
  return *(uint32_t *)vec_get(&s->defs.kind_row, d.idx);
}

// =====================================================================
// Per-fn reads — gated by KIND_FUNCTION inside the wrappers.
// =====================================================================
//
// The producing query for each column:
//   - FN_SIGNATURE → fns.signature_result   (db_query_fn_signature)
//   - INFER_BODY   → fns.body_node_types    (db_query_infer_body)
//                    fns.body_ast_id_maps   (built by infer_body too)
//   - BODY_SCOPES  → fns.body               (db_query_body_scopes)
//
// Each wrapper calls the producing query (records the dep) and returns
// the column. If the def isn't a function, returns NULL / zero range.

const FnSignature *db_read_fn_signature(db_query_ctx *ctx, DefId d) {
  struct db *s = (struct db *)ctx;
  assert(db_query_stack_top(s) != NULL &&
         "db_read_fn_signature: must be called from inside a query frame.");
  if (d.idx == 0)
    return NULL;
  return db_query_fn_signature(ctx, d);
}

NodeTypesRange db_read_fn_body_node_types(db_query_ctx *ctx, DefId d) {
  struct db *s = (struct db *)ctx;
  (void)s;
  assert(db_query_stack_top(s) != NULL &&
         "db_read_fn_body_node_types: must be called from inside a query frame.");
  if (d.idx == 0)
    return (NodeTypesRange){0};
  return db_query_infer_body(ctx, d);
}

const FnBody *db_read_fn_body_scopes(db_query_ctx *ctx, DefId d) {
  struct db *s = (struct db *)ctx;
  assert(db_query_stack_top(s) != NULL &&
         "db_read_fn_body_scopes: must be called from inside a query frame.");
  if (d.idx == 0)
    return NULL;
  return db_query_body_scopes(ctx, d);
}

const BodyAstIdMap *db_read_fn_body_ast_id_map(db_query_ctx *ctx, DefId d) {
  struct db *s = (struct db *)ctx;
  assert(db_query_stack_top(s) != NULL &&
         "db_read_fn_body_ast_id_map: must be called from inside a query frame.");
  if (d.idx == 0)
    return NULL;
  // INFER_BODY populates body_ast_id_maps as a side effect. Touching
  // it records the dep.
  (void)db_query_infer_body(ctx, d);
  if (d.idx >= s->defs.kind_row.count)
    return NULL;
  DefKind k = *(DefKind *)vec_get(&s->defs.kinds, d.idx);
  if (k != KIND_FUNCTION)
    return NULL;
  uint32_t row = *(uint32_t *)vec_get(&s->defs.kind_row, d.idx);
  if (row >= paged_count(&s->fns.body_ast_id_maps))
    return NULL;
  return (const BodyAstIdMap *)paged_get(&s->fns.body_ast_id_maps, row);
}

// =====================================================================
// Per-file reads.
// =====================================================================

struct GreenNode *db_read_file_ast(db_query_ctx *ctx, FileId fid) {
  struct db *s = (struct db *)ctx;
  (void)s;
  assert(db_query_stack_top(s) != NULL &&
         "db_read_file_ast: must be called from inside a query frame.");
  return db_query_file_ast(ctx, fid);
}

// =====================================================================
// Per-namespace member-list reads — dep on NAMESPACE_TYPE.
// =====================================================================
//
// Fire NAMESPACE_TYPE first (records the dep on the caller's frame),
// then read the field_lo / field_len cell via the existing db.h
// inline getter. The cell is owned by NAMESPACE_TYPE; the producing
// query rebuilds it whenever the namespace's member list changes,
// which is exactly when our cached dep should invalidate.

extern IpIndex db_query_namespace_type(db_query_ctx *ctx, NamespaceId nsid);

uint32_t db_read_namespace_member_count(db_query_ctx *ctx, NamespaceId n) {
  struct db *s = (struct db *)ctx;
  assert(db_query_stack_top(s) != NULL &&
         "db_read_namespace_member_count: must be called from inside a "
         "query frame. Driver-level callers use db_namespace_member_count.");
  (void)db_query_namespace_type(ctx, n); // dep recorded
  return db_namespace_member_count(s, n);
}

DeclEntry db_read_namespace_member_at(db_query_ctx *ctx, NamespaceId n,
                                      uint32_t i) {
  struct db *s = (struct db *)ctx;
  assert(db_query_stack_top(s) != NULL &&
         "db_read_namespace_member_at: must be called from inside a "
         "query frame. Driver-level callers use db_namespace_member_at.");
  (void)db_query_namespace_type(ctx, n); // dep recorded
  return db_namespace_member_at(s, n, i);
}

// =====================================================================
// Untracked variants — driver-level / content-addressed.
// =====================================================================
//
// No assert, no dep recording, no producing-query call. Caller is the
// owner of the value's computation or knows the value is invariant.

DefKind db_get_def_kind_untracked(struct db *s, DefId d) {
  if (d.idx == 0 || d.idx >= s->defs.kinds.count)
    return KIND_NONE;
  return *(DefKind *)vec_get(&s->defs.kinds, d.idx);
}

StrId db_get_def_name_untracked(struct db *s, DefId d) {
  if (d.idx == 0 || d.idx >= s->defs.names.count)
    return (StrId){0};
  return *(StrId *)vec_get(&s->defs.names, d.idx);
}

NamespaceId db_get_def_parent_module_untracked(struct db *s, DefId d) {
  if (d.idx == 0 || d.idx >= s->defs.parent_modules.count)
    return (NamespaceId){0};
  return *(NamespaceId *)vec_get(&s->defs.parent_modules, d.idx);
}

uint32_t db_get_def_kind_row_untracked(struct db *s, DefId d) {
  if (d.idx == 0 || d.idx >= s->defs.kind_row.count)
    return 0;
  return *(uint32_t *)vec_get(&s->defs.kind_row, d.idx);
}

uint32_t db_get_def_count_untracked(struct db *s) {
  return (uint32_t)s->defs.names.count;
}

bool db_def_id_valid_untracked(struct db *s, DefId d) {
  return d.idx != 0 && d.idx < s->defs.names.count;
}
