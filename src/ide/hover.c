// Hover — type description for the cursor position.
//
// Pure read of typecheck state via the node→type router
// (db_query_node_type, src/db/query/node_type.c): it finds the enclosing
// decl, ensures that decl's per-decl queries, and looks the node up across
// the infer_body / fn_signature / type_of_def ranges, with a top-level
// resolve_ref fallback. Wrapped in a request boundary so those queries pin a
// consistent revision (they cache-hit off the immediately-preceding typecheck).

#include "ide.h"

#include "../ast/ast_decl.h"
#include "../ast/ast_expr.h"
#include "../db/db.h"
#include "../syntax/syntax_kind.h"
#include "../support/data_structure/stringpool.h"
#include "../syntax/syntax.h"

#include <stdio.h>
#include <string.h>

// Node→type router (no per-query header post-D1; db_query_ctx == struct db).
extern IpIndex db_query_node_type(db_query_ctx *ctx, FileId fid,
                                  SyntaxNode *node);

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

  db_request_begin(db, db_current_revision(db));

  // Name for the "name: type" display, by node kind. (The TYPE comes from the
  // router below, uniformly — it handles refs, locals, params, fields, and the
  // top-level resolve_ref fallback.)
  StrId name_id = {0};
  switch (syntax_node_kind(node)) {
  case SK_REF_EXPR: {
    RefExpr r;
    if (RefExpr_cast(node, &r)) {
      SyntaxToken *nt = RefExpr_name(&r);
      name_id = intern_tok(db, nt);
      if (nt) syntax_token_release(nt);
    }
    break;
  }
  case SK_CONST_DECL: {
    ConstDef cd;
    if (ConstDef_cast(node, &cd)) {
      SyntaxToken *nt = ConstDef_name(&cd);
      name_id = intern_tok(db, nt);
      if (nt) syntax_token_release(nt);
    }
    break;
  }
  case SK_VAR_DECL: {
    VarDef vd;
    if (VarDef_cast(node, &vd)) {
      SyntaxToken *nt = VarDef_name(&vd);
      name_id = intern_tok(db, nt);
      if (nt) syntax_token_release(nt);
    }
    break;
  }
  case SK_PARAM: {
    Param p;
    if (Param_cast(node, &p)) {
      SyntaxToken *nt = Param_name(&p);
      name_id = intern_tok(db, nt);
      if (nt) syntax_token_release(nt);
    }
    break;
  }
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

  IpIndex type = db_query_node_type(db, fid, node);
  db_request_end(db);

  const char *name_str =
      (name_id.idx != 0) ? pool_get(&db->strings, name_id) : NULL;
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
