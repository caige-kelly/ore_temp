// Hover — type description for the cursor position.
//
// Architecture: pure read of typecheck state. Reads `node_data.types[]`
// populated by sema_type_of_expr's wrapper during the immediately-
// preceding compile_file run (oredb_typecheck). Does NOT call
// sema_type_of_expr — that would emit diagnostics through the
// centralized diag pipeline, which asserts on an active query frame.
//
// Wraps the body in db_request_begin/end because db_query_resolve_ref
// and db_query_type_of_def (called via resolve_path_for_hover) need a
// pinned effective_revision. The dependent slots were already
// populated by the prior typecheck so the queries cache-hit; the
// request boundary is for soundness, not work.

#include "ide.h"

#include "../db/db.h"
#include "../db/intern_pool/intern_pool.h"
#include "../db/query/node_type.h"
#include "../db/query/resolve_ref.h"
#include "../db/query/type_of_def.h"
#include "../parser/ast.h"
#include "../sema/sema.h"

#include <stdio.h>
#include <string.h>

// Enclosing top-level DefId via the parent-chain walk stamped by
// module_exports. O(parent_depth).
static DefId enclosing_fn_for_node(struct db *s, FileId fid, AstNodeId node) {
  return db_get_def_for_node(s, fid, node);
}

// Resolve a name in path position. Body scope first (when we know the
// enclosing fn), then namespace internal scope. Returns IP_NONE on
// unresolved. Primitives (u8, usize, bool, ...) are now real DefIds in
// each namespace's parent scope (db_init_primitives), so the namespace
// resolve_ref step finds them without any dedicated fallback.
static IpIndex resolve_path_for_hover(struct db *s, NamespaceId nsid,
                                      DefId enclosing_fn, AstNodeId use_node,
                                      StrId name) {
  if (name.idx == 0)
    return IP_NONE;
  if (def_id_valid(enclosing_fn)) {
    // Hover prefers ANY type info: a local bind with IP_NONE type
    // tells us nothing useful, so fall through to namespace scope
    // (which may have a more informative answer for a shadowed
    // top-level name). Pass NULL for found_out — hover doesn't
    // emit undefined-identifier diagnostics.
    IpIndex local =
        sema_body_scope_lookup(s, enclosing_fn, use_node, name, NULL);
    if (local.v != IP_NONE.v)
      return local;
  }
  ScopeId internal = db_get_namespace_internal_scope(s, nsid);
  if (internal.idx != SCOPE_ID_NONE.idx) {
    DefId target = db_query_resolve_ref(s, internal, name);
    if (def_id_valid(target))
      return db_query_type_of_def(s, target);
  }
  return IP_NONE;
}

size_t ide_hover_at(struct db *db, FileId fid, uint32_t line0, uint32_t char0,
                    char *buf, size_t buflen) {
  if (!buf || buflen == 0)
    return 0;
  buf[0] = '\0';

  if (!file_id_valid(fid))
    return 0;

  uint32_t off = db_byte_offset_at(db, fid, line0, char0);
  if (off == UINT32_MAX)
    return 0;
  AstNodeId node = db_node_at_offset(db, fid, off);
  if (node.idx == AST_NODE_ID_NONE.idx)
    return 0;

  NamespaceId nsid = db_get_file_namespace(db, fid);
  if (!namespace_id_valid(nsid))
    return 0;

  ASTStore *ast = db_get_file_ast(db, fid);
  if (!ast)
    return 0;

  AstNodeKind k = ((AstNodeKind *)ast->kinds.data)[node.idx];
  AstNodeData d = ((AstNodeData *)ast->data.data)[node.idx];

  db_request_begin(db, db_current_revision(db));
  DefId enclosing_fn = enclosing_fn_for_node(db, fid, node);

  IpIndex type = IP_NONE;
  const char *name_str = NULL;

  switch (k) {
  case AST_EXPR_PATH: {
    // Use-site identifier: prefer the per-node cache populated by
    // sema_check_expr's bidirectional pass. That cache holds the
    // contextual coerced type — so a comptime ref used in i32 context
    // hovers as i32, not as the def's stored comptime_int.
    // resolve_path_for_hover (which returns db_query_type_of_def) is
    // the fallback when the per-node cache is empty (e.g., refs outside
    // any builder-active context).
    StrId name = d.string_id;
    name_str = pool_get(&db->strings, name);
    type = db_query_node_type(db, fid, node);
    if (type.v == IP_NONE.v)
      type = resolve_path_for_hover(db, nsid, enclosing_fn, node, name);
    break;
  }
  // Top-level decl names hover as the decl's type. The "cursor on
  // the name token" case lands on the AST_DECL_* node because the
  // name's span is part of the decl's span (innermost-containing
  // wins).
  case AST_DECL_CONST:
  case AST_DECL_VAR: {
    const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
    StrId name = {.idx = ex[0]};
    name_str = pool_get(&db->strings, name);
    if (def_id_valid(enclosing_fn))
      type = sema_body_scope_lookup(db, enclosing_fn, node, name, NULL);
    if (type.v == IP_NONE.v)
      type = resolve_path_for_hover(db, nsid, DEF_ID_NONE, node, name);
    break;
  }
  // Fn parameter. AST_DECL_PARAM extras: [name_strid, type_id, ...].
  // Per-decl salsa queries (db_query_fn_signature, db_query_infer_body)
  // own NodeTypesRanges in db.node_types_pool; the unified router
  // (db_query_node_type) reads from those ranges in the default
  // branch below. This case just extracts the name for display.
  // The signature scope from sema_body_scopes covers name LOOKUP
  // from signature positions for the (rare) case of dependent param
  // types referencing sibling params.
  case AST_DECL_PARAM: {
    const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
    StrId name = {.idx = ex[0]};
    name_str = pool_get(&db->strings, name);
    break; // type comes from the default-branch cache read below
  }
  // Struct / union field. AST_DECL_FIELD extras: [name_strid, type_id,
  // vis, fpos]. build_struct_type stamps the field node's type into
  // the per-node cache (L2), so the default-branch read picks it up.
  case AST_DECL_FIELD: {
    const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
    StrId name = {.idx = ex[0]};
    name_str = pool_get(&db->strings, name);
    break; // type comes from the default-branch cache read below
  }
  default:
    break;
  }

  // Unified node→type router. Salsa-driven: walks parents to find
  // the enclosing def, drives the per-decl queries that own ranges
  // of node types (infer_body / fn_signature / struct field_types),
  // returns the resolved IpIndex. Replaces the legacy direct read
  // of FileNodeData.types[node.idx], which the Option-C migration
  // demolishes alongside the post-typecheck walker.
  if (type.v == IP_NONE.v)
    type = db_query_node_type(db, fid, node);

  db_request_end(db);

  if (type.v == IP_NONE.v && (!name_str || !name_str[0]))
    return 0;

  char tbuf[256];
  db_format_type(db, type, tbuf, sizeof tbuf);

  int n;
  if (name_str && name_str[0])
    n = snprintf(buf, buflen, "%s: %s", name_str, tbuf);
  else
    n = snprintf(buf, buflen, "%s", tbuf);
  return n < 0 ? 0 : (size_t)n;
}
