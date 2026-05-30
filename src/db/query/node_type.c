// NODE-TYPE ROUTER — the consumer-facing "type at this syntax node" helper.
//
// IDE features (hover, completion) need the resolved type of an arbitrary
// SyntaxNode. The per-node types live in several memoized per-decl results:
//   - a fn body's node types → db_query_infer_body's NodeTypesRange
//   - a fn signature's param/return types → fn_signature.node_types
//   - a struct's field-annotation types → type_of_def's field_node_types
//   - a const/var value's types → type_of_def's value_node_types
// This router finds the ENCLOSING top-level decl, ensures the right query ran,
// and looks the node up across those ranges. It is a PLAIN function composing
// memoized queries (not itself memoized, like db_check_namespace); the caller
// owns the request boundary. Reconstructs the deleted db_get_def_for_node +
// db_query_node_type that the consumers were rewritten off of in D1.

#define ORE_ENGINE_PRIVATE
#include "../db.h"
#include "engine.h"
#include "result_columns.h" // *_node_types_read helpers
#include "type_layer.h"     // node_types_range_lookup

#include "../../ast/ast_decl.h" // ConstDef / VarDef
#include "../../ast/ast_expr.h" // RefExpr
#include "../../support/data_structure/stringpool.h"
#include "../../syntax/syntax.h"
#include "../../syntax/syntax_kind.h"

extern FileArray db_query_namespace_items(db_query_ctx *, NamespaceId);
extern DefId db_query_def_identity(db_query_ctx *, NamespaceId, AstId);
extern IpIndex db_query_type_of_def(db_query_ctx *, DefId);
extern const FnSignature *db_query_fn_signature(db_query_ctx *, DefId);
extern NodeTypesRange db_query_infer_body(db_query_ctx *, DefId);
extern DefId db_query_resolve_ref(db_query_ctx *, ScopeId, StrId);
extern NamespaceScopes db_query_namespace_scopes(db_query_ctx *, NamespaceId);

// Name token of a top-level decl node (ConstDef / VarDef wrapper), {0} if it
// isn't a named bind.
static StrId decl_name_of(struct db *s, SyntaxNode *decl) {
  SyntaxKind k = syntax_node_kind(decl);
  SyntaxToken *nt = NULL;
  if (k == SK_CONST_DECL) {
    ConstDef cd;
    if (ConstDef_cast(decl, &cd))
      nt = ConstDef_name(&cd);
  } else if (k == SK_VAR_DECL) {
    VarDef vd;
    if (VarDef_cast(decl, &vd))
      nt = VarDef_name(&vd);
  }
  if (!nt)
    return (StrId){0};
  StrId name = pool_intern(&s->strings, syntax_token_text(nt),
                           syntax_token_text_range(nt).length);
  syntax_token_release(nt);
  return name;
}

// The DefId of the top-level decl that lexically encloses `node` (the decl
// whose parent is the file root). DEF_ID_NONE if none / unnamed.
DefId db_node_enclosing_def(db_query_ctx *ctx, FileId fid, SyntaxNode *node) {
  struct db *s = (struct db *)ctx;
  if (!node)
    return DEF_ID_NONE;

  // Ascend to the highest non-root ancestor. `cur` trails one level below
  // `parent`; when `parent` is the root, `cur` is the top-level decl.
  // syntax_node_parent RETURNS_OWNED (+1; NULL at root), so release as we go.
  SyntaxNode *parent = syntax_node_parent(node); // +1 or NULL
  if (!parent)
    return DEF_ID_NONE;   // node IS the root
  SyntaxNode *cur = node; // borrowed (caller's)
  bool cur_owned = false;
  for (;;) {
    SyntaxNode *grand = syntax_node_parent(parent); // +1 or NULL
    if (!grand) { // parent is the root → cur is the decl
      syntax_node_release(parent);
      break;
    }
    if (cur_owned)
      syntax_node_release(cur);
    cur = parent;
    cur_owned = true; // cur takes parent's ref
    parent = grand;   // parent takes grand's ref
  }

  StrId name = decl_name_of(s, cur);
  if (cur_owned)
    syntax_node_release(cur);
  if (name.idx == 0)
    return DEF_ID_NONE;

  NamespaceId nsid = db_get_file_namespace(s, fid);
  if (!namespace_id_valid(nsid))
    return DEF_ID_NONE;
  FileArray items = db_query_namespace_items(ctx, nsid);
  const NamespaceItem *a = (const NamespaceItem *)items.data;
  for (uint32_t i = 0; i < items.count; i++)
    if (a[i].name.idx == name.idx)
      return db_query_def_identity(ctx, nsid, a[i].id);
  return DEF_ID_NONE;
}

// The resolved type at `node`, IP_NONE if unknown. Ensures the enclosing
// decl's per-node ranges this revision, then looks `node` up across them;
// falls back to top-level name resolution for a bare ref.
IpIndex db_query_node_type(db_query_ctx *ctx, FileId fid, SyntaxNode *node) {
  struct db *s = (struct db *)ctx;
  if (!node)
    return IP_NONE;

  DefId d = db_node_enclosing_def(ctx, fid, node);
  if (d.idx != 0) {
    DefKind k = db_def_kind(s, d);
    if (k == KIND_FUNCTION) {
      (void)db_query_fn_signature(ctx, d); // ensure sig range
      NodeTypesRange body = db_query_infer_body(ctx, d);
      IpIndex t = node_types_range_lookup(s, body, node);
      if (t.v != IP_NONE.v)
        return t;
      const FnSignature *sig = fn_signature_read(s, d);
      if (sig) {
        t = node_types_range_lookup(s, sig->node_types, node);
        if (t.v != IP_NONE.v)
          return t;
      }
    } else {
      (void)db_query_type_of_def(ctx, d); // ensure embedded range
      NodeTypesRange r = {0};
      switch (k) {
      case KIND_STRUCT:
        r = struct_field_node_types_read(s, d);
        break;
      case KIND_VARIABLE:
        r = variable_value_node_types_read(s, d);
        break;
      case KIND_CONSTANT:
        r = constant_value_node_types_read(s, d);
        break;
      default:
        break;
      }
      IpIndex t = node_types_range_lookup(s, r, node);
      if (t.v != IP_NONE.v)
        return t;
    }
  }

  // Fallback: a bare reference to a top-level name (incl. hovering a decl's
  // own name) that no per-node range captured → resolve in the namespace
  // internal scope and report the def's type.
  if (syntax_node_kind(node) == SK_REF_EXPR) {
    RefExpr r;
    if (RefExpr_cast(node, &r)) {
      SyntaxToken *nt = RefExpr_name(&r);
      if (nt) {
        StrId name = pool_intern(&s->strings, syntax_token_text(nt),
                                 syntax_token_text_range(nt).length);
        syntax_token_release(nt);
        NamespaceId nsid = db_get_file_namespace(s, fid);
        if (name.idx != 0 && namespace_id_valid(nsid)) {
          ScopeId internal = db_query_namespace_scopes(ctx, nsid).internal;
          if (internal.idx != SCOPE_ID_NONE.idx) {
            DefId target = db_query_resolve_ref(ctx, internal, name);
            if (target.idx != 0)
              return db_query_type_of_def(ctx, target);
          }
        }
      }
    }
  }
  return IP_NONE;
}
