#include "../ast/ast_decl.h"
#include "../ast/ast_expr.h"
#include "../db/db.h"
#include "../db/intern_pool/intern_pool.h"
#include "../db/query/body_scopes.h"
#include "../db/query/decl_ast.h"
#include "../db/query/def_identity.h"
#include "../db/query/fn_signature.h"
#include "../db/query/index.h"
#include "../parser/syntax_kind.h"
#include "../syntax/syntax.h"
#include "sema.h"

// Body type inference. With body_scopes now its own query, this
// function's job is just: (1) declare a dep on body_scopes(def) so its
// result is available to type_of_expr's path lookups, (2) bidirectionally
// check the body against the declared return type, (3) accumulate the
// typed-body fingerprint over every node typed during the check.
//
// All bind collection moved to sema/body_scopes.c.
IpIndex sema_infer_body(struct db *s, DefId def, Fingerprint *body_fp_out) {
  SyntaxNodePtr ptr = *(SyntaxNodePtr *)vec_get(&s->defs.syntax_ptrs, def.idx);
  NamespaceId nsid = *(NamespaceId *)vec_get(&s->defs.parent_modules, def.idx);

  // Depend on the module's top-level index — this body reads the
  // module's file list below; a file-set change must re-run it.
  (void)db_query_top_level_index(s, nsid);
  (void)db_query_def_identity(s, nsid, ptr);
  IpIndex sig = db_query_fn_signature(s, def);
  if (sig.v == IP_NONE.v)
    return IP_NONE;

  // Records dep on body_scopes(def) — any AST edit that reshapes the
  // body's binding structure (or changes a referenced decl's type)
  // invalidates this query. type_of_expr's REF handler reads the
  // body-scope pools raw under the umbrella of this dep.
  (void)db_query_body_scopes(s, def);

  // Locate the lambda + body node to drive the return-type check.
  SyntaxTree *body_tree = NULL;
  SyntaxNode *lambda_node = NULL;
  FileId body_fid = FILE_ID_NONE;
  GreenNode *body_groot = NULL;
  uint32_t fc = 0;
  const FileId *files = db_get_namespace_files(s, nsid, &fc);
  for (uint32_t i = 0; i < fc; i++) {
    // Per-decl AST dep — see type_of_def.c.
    SyntaxNodePtr cur_ptr = db_query_decl_ast(s, files[i], ptr);
    if (cur_ptr.kind == SYNTAX_KIND_NONE)
      continue;
    uint32_t local = file_id_local(files[i]);
    GreenNode *groot =
        *(GreenNode **)vec_get(&s->files.green_roots, local);
    if (!groot)
      continue;
    SyntaxTree *tree = syntax_tree_new(groot);
    SyntaxNode *root = syntax_tree_root(tree);
    SyntaxNode *decl = syntax_node_ptr_resolve(cur_ptr, root);
    syntax_node_release(root);
    if (!decl) {
      syntax_tree_free(tree);
      continue;
    }
    SyntaxKind dk = syntax_node_kind(decl);
    if (dk != SK_CONST_DECL && dk != SK_VAR_DECL) {
      syntax_node_release(decl);
      syntax_tree_free(tree);
      break;
    }
    SyntaxNode *value = NULL;
    if (dk == SK_CONST_DECL) {
      ConstDef cd;
      if (ConstDef_cast(decl, &cd)) value = ConstDef_value(&cd);
    } else {
      VarDef vd;
      if (VarDef_cast(decl, &vd)) value = VarDef_value(&vd);
    }
    syntax_node_release(decl);
    if (!value) {
      syntax_tree_free(tree);
      break;
    }
    if (syntax_node_kind(value) != SK_LAMBDA_EXPR) {
      syntax_node_release(value);
      syntax_tree_free(tree);
      break;
    }
    body_tree = tree;
    lambda_node = value;
    body_fid = files[i];
    body_groot = groot;
    break;
  }

  if (!body_tree || !lambda_node)
    return sig;

  LambdaExpr lam;
  if (!LambdaExpr_cast(lambda_node, &lam)) {
    syntax_node_release(lambda_node);
    syntax_tree_free(body_tree);
    return sig;
  }
  SyntaxNode *body_node = LambdaExpr_body(&lam);

  // Open a NodeTypeBuilder over the body. sema_check_expr and its
  // recursive sema_type_of_expr calls push every visited node's
  // resolved type into this builder; on completion the assembled
  // NodeTypesRange lands on db.fns.body_node_types[fn_row] for the
  // unified node_type router to read. Mirrors rust-analyzer's per-body
  // InferenceResult — the query body owns the map, no side-effect
  // walker repopulation.
  NodeTypeBuilder body_b;
  sema_node_type_builder_begin(s, &body_b, body_fid);

  SemaCtx body_ctx = {
      .s               = s,
      .file_green_root = body_groot,
      .nsid            = nsid,
      .enclosing_fn    = def,
      .file_local      = body_fid,
      .types           = &body_b,
  };

  if (body_node) {
    IpKey sig_key = ip_key(&s->intern, sig);
    IpIndex expected_ret = sig_key.fn_type.ret;
    (void)sema_check_expr(&body_ctx, body_node, expected_ret);
  }

  Fingerprint body_range_fp = 0;
  NodeTypesRange body_range =
      sema_node_type_builder_end(&body_b, &body_range_fp);

  // Stash the range on db.fns.body_node_types[fn_row] so the router
  // (db_query_node_type) can read it without re-running this query.
  // On re-runs the previous range's HashMap leaks unless freed first —
  // free the old map, then move the new range in.
  if (db_def_kind(s, def) == KIND_FUNCTION) {
    uint32_t row = db_def_row(s, def, KIND_FUNCTION);
    NodeTypesRange *slot =
        (NodeTypesRange *)vec_get(&s->fns.body_node_types, row);
    if (hashmap_is_initialized(&slot->types))
      hashmap_free(&slot->types);
    *slot = body_range;
  } else {
    // No place to stash — free immediately to avoid a leak.
    if (hashmap_is_initialized(&body_range.types))
      hashmap_free(&body_range.types);
  }

  // Typed-body fingerprint. The NodeTypeBuilder accumulated a
  // (syntax_node_ptr_hash, type.v) fold over every body node visited;
  // we receive it from builder_end. Stable across sibling edits and
  // reparses that don't change body types — consumers early-cutoff
  // accordingly.
  if (body_fp_out)
    *body_fp_out = body_range_fp;

  if (body_node) syntax_node_release(body_node);
  syntax_node_release(lambda_node);
  syntax_tree_free(body_tree);

  return sig;
}
