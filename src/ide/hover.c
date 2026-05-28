// Hover — type description for the cursor position.
//
// Architecture: pure read of typecheck state. Reads the per-decl
// NodeTypesRange HashMaps populated by sema_type_of_expr / sema_check_expr
// during the immediately-preceding compile_file run (oredb_typecheck).
// Does NOT call sema_type_of_expr — that would emit diagnostics through
// the centralized diag pipeline, which asserts on an active query frame.
//
// Wraps the body in db_request_begin/end because db_query_resolve_ref
// and db_query_type_of_def (called via resolve_path_for_hover) need a
// pinned effective_revision. Dependent slots were already populated by
// the prior typecheck so the queries cache-hit; the request boundary is
// for soundness, not work.

#include "ide.h"

#include "../ast/ast_decl.h"
#include "../ast/ast_expr.h"
#include "../db/db.h"
#include "../db/intern_pool/intern_pool.h"
#include "../db/query/node_type.h"
#include "../db/query/resolve_ref.h"
#include "../db/query/type_of_def.h"
#include "../syntax/syntax_kind.h"
#include "../sema/sema.h"
#include "../support/data_structure/stringpool.h"
#include "../syntax/syntax.h"

#include <stdio.h>
#include <string.h>

// Resolve a name in path position. Body scope first (when we know the
// enclosing fn), then namespace internal scope. Returns IP_NONE on
// unresolved. Primitives (u8, usize, bool, ...) are real DefIds in
// each namespace's parent scope (db_init_primitives), so the namespace
// resolve_ref step finds them without any dedicated fallback.
static IpIndex resolve_path_for_hover(struct db *s, NamespaceId nsid,
                                      DefId enclosing_fn,
                                      SyntaxNode *use_node, StrId name) {
  if (name.idx == 0)
    return IP_NONE;
  if (def_id_valid(enclosing_fn)) {
    // Hover prefers ANY type info: a local bind with IP_NONE type
    // tells us nothing useful, so fall through to namespace scope.
    // Pass NULL for found_out — hover doesn't emit "undefined" diags.
    IpIndex local =
        sema_body_scope_lookup(s, enclosing_fn, use_node, name, NULL);
    if (local.v != IP_NONE.v)
      return local;
  }
  // TODO(phase-D): route through db_query_namespace_scopes result column.
  ScopeId internal = db_get_namespace_internal_scope(s, nsid);
  if (internal.idx != SCOPE_ID_NONE.idx) {
    DefId target = db_query_resolve_ref(s, internal, name);
    if (def_id_valid(target))
      return db_query_type_of_def(s, target);
  }
  return IP_NONE;
}

// Intern a SyntaxToken's text. Returns {0} on NULL.
static StrId intern_tok(struct db *s, SyntaxToken *t) {
  if (!t)
    return (StrId){0};
  const char *txt = syntax_token_text(t);
  uint32_t len = syntax_token_text_range(t).length;
  return pool_intern(&s->strings, txt, len);
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
  SyntaxNode *node = db_node_at_offset(db, fid, off);
  if (!node)
    return 0;

  NamespaceId nsid = db_get_file_namespace(db, fid);
  if (!namespace_id_valid(nsid)) {
    syntax_node_release(node);
    return 0;
  }

  db_request_begin(db, db_current_revision(db));
  // TODO(phase-D): db_get_def_for_node deleted in B1 (it called the
  // long-removed QUERY_NODE_TO_DECL). Reimplement via parent-chain
  // walk + db_query_def_identity per-entry once hover/sema are
  // rewritten on the new engine.
  DefId enclosing_fn = db_get_def_for_node(db, fid, node);

  IpIndex type = IP_NONE;
  StrId name_id = {0};

  SyntaxKind k = syntax_node_kind(node);
  switch (k) {
  case SK_REF_EXPR: {
    // Use-site identifier: prefer the per-node cache populated by
    // sema_check_expr's bidirectional pass. That cache holds the
    // contextual coerced type — so a comptime ref used in i32 context
    // hovers as i32, not as the def's stored comptime_int.
    // resolve_path_for_hover (which returns db_query_type_of_def) is
    // the fallback when the per-node cache is empty.
    RefExpr r;
    if (RefExpr_cast(node, &r)) {
      SyntaxToken *nt = RefExpr_name(&r);
      name_id = intern_tok(db, nt);
      if (nt) syntax_token_release(nt);
    }
    type = db_query_node_type(db, fid, node);
    if (type.v == IP_NONE.v)
      type =
          resolve_path_for_hover(db, nsid, enclosing_fn, node, name_id);
    break;
  }
  // Top-level decl names hover as the decl's type. The "cursor on the
  // name token" case lands on the SK_*_DECL node because the name's
  // span is part of the decl's span (innermost-containing wins).
  case SK_CONST_DECL: {
    ConstDef cd;
    if (ConstDef_cast(node, &cd)) {
      SyntaxToken *nt = ConstDef_name(&cd);
      name_id = intern_tok(db, nt);
      if (nt) syntax_token_release(nt);
    }
    if (def_id_valid(enclosing_fn))
      type = sema_body_scope_lookup(db, enclosing_fn, node, name_id, NULL);
    if (type.v == IP_NONE.v)
      type = resolve_path_for_hover(db, nsid, DEF_ID_NONE, node, name_id);
    break;
  }
  case SK_VAR_DECL: {
    VarDef vd;
    if (VarDef_cast(node, &vd)) {
      SyntaxToken *nt = VarDef_name(&vd);
      name_id = intern_tok(db, nt);
      if (nt) syntax_token_release(nt);
    }
    if (def_id_valid(enclosing_fn))
      type = sema_body_scope_lookup(db, enclosing_fn, node, name_id, NULL);
    if (type.v == IP_NONE.v)
      type = resolve_path_for_hover(db, nsid, DEF_ID_NONE, node, name_id);
    break;
  }
  // Fn parameter — name comes from the wrapper; type comes from the
  // default-branch cache read below (signature_node_types).
  case SK_PARAM: {
    Param p;
    if (Param_cast(node, &p)) {
      SyntaxToken *nt = Param_name(&p);
      name_id = intern_tok(db, nt);
      if (nt) syntax_token_release(nt);
    }
    break;
  }
  // Struct / union field — same shape as Param: name from wrapper,
  // type from the per-node cache.
  case SK_FIELD: {
    Field f;
    if (Field_cast(node, &f)) {
      SyntaxToken *nt = Field_name(&f);
      name_id = intern_tok(db, nt);
      if (nt) syntax_token_release(nt);
    }
    break;
  }
  default:
    break;
  }

  // Unified node→type router. Salsa-driven: walks parents to find the
  // enclosing def, drives the per-decl queries that own per-node type
  // ranges (infer_body / fn_signature / type_of_def), returns the
  // resolved IpIndex.
  if (type.v == IP_NONE.v)
    type = db_query_node_type(db, fid, node);

  db_request_end(db);

  const char *name_str = (name_id.idx != 0) ? pool_get(&db->strings, name_id)
                                            : NULL;

  if (type.v == IP_NONE.v && (!name_str || !name_str[0])) {
    syntax_node_release(node);
    return 0;
  }

  char tbuf[256];
  db_format_type(db, type, tbuf, sizeof tbuf);

  int n;
  if (name_str && name_str[0])
    n = snprintf(buf, buflen, "%s: %s", name_str, tbuf);
  else
    n = snprintf(buf, buflen, "%s", tbuf);

  syntax_node_release(node);
  return n < 0 ? 0 : (size_t)n;
}
