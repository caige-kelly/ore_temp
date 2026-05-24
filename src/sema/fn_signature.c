#include "../db/db.h"
#include "../db/intern_pool/intern_pool.h"
#include "../db/query/decl_ast.h"
#include "../db/query/def_identity.h"
#include "../db/query/index.h"
#include "../parser/ast.h"
#include "sema.h"

// Build an interned fn type from a param-id array + return-type node.
// Shared by the lambda-RHS path (sema_fn_signature) and the type-
// position path (sema_resolve_type_expr's AST_TYPE_FN case). Identity
// is purely structural — (ret, modifiers, params) — so an anonymous
// fn type in a field dedups with a top-level fn that shares its shape.
//
// Scratch params array comes from db.request_arena (reset at
// db_request_end), so n_params has no compile-time cap.
//
// ctx threads s/ast/nsid/file_local + the active NodeTypeBuilder for
// per-node cache writes (param type-exprs + param decl nodes themselves).
IpIndex sema_build_fn_type(const SemaCtx *ctx, AstNodeId ret_node,
                           const uint32_t *param_ids, uint32_t n_params) {
  struct db *s = ctx->s;
  ASTStore *ast = ctx->ast;
  IpIndex *params = NULL;
  if (n_params > 0) {
    params = arena_alloc(&s->request_arena, n_params * sizeof(IpIndex));
    if (!params)
      return IP_NONE;
  }
  for (uint32_t i = 0; i < n_params; i++) {
    AstNodeId param_id = {.idx = param_ids[i]};
    if (param_id.idx == AST_NODE_ID_NONE.idx)
      return IP_NONE;
    AstNodeKind pk = ((AstNodeKind *)ast->kinds.data)[param_id.idx];
    if (pk != AST_DECL_PARAM)
      return IP_NONE;
    AstNodeData pd = ((AstNodeData *)ast->data.data)[param_id.idx];
    const uint32_t *pex = &((uint32_t *)ast->extra.data)[pd.extra_idx.idx];
    AstNodeId ptype = {.idx = pex[1]};
    IpIndex pti = sema_resolve_type_expr(ctx, ptype);
    if (pti.v == IP_NONE.v)
      return IP_NONE;
    params[i] = pti;
    // Stamp the AST_DECL_PARAM node itself with the param's type, so
    // hover on the param-name token reads it from the cache directly.
    sema_node_type_builder_push(ctx, param_id, pti);
  }

  IpIndex ret;
  if (ret_node.idx == AST_NODE_ID_NONE.idx) {
    ret = IP_VOID_TYPE; // implicit void on a missing return-type slot
  } else {
    ret = sema_resolve_type_expr(ctx, ret_node);
    if (ret.v == IP_NONE.v)
      return IP_NONE;
  }

  IpKey key = {.kind = IPK_FN_TYPE,
               .fn_type = {.ret = ret,
                           .modifiers = 0,
                           .params = params,
                           .n_params = n_params},
               // params is borrowed from request_arena — stamp it so ip_get can
               // assert the key is consumed before the next request reset.
               .src_arena = params ? &s->request_arena : NULL,
               .src_gen = s->request_arena.generation};
  return ip_get(&s->intern, key);
}

IpIndex sema_fn_signature(struct db *s, DefId def) {
  AstId ast_id = *(AstId *)vec_get(&s->defs.ast_ids, def.idx);
  NamespaceId nsid = *(NamespaceId *)vec_get(&s->defs.parent_modules, def.idx);

  // Depend on the module's top-level index — this body reads the
  // module's file list below; a file-set change must re-run it.
  (void)db_query_top_level_index(s, nsid);
  (void)db_query_def_identity(s, nsid, ast_id);

  uint32_t fc = 0;
  const FileId *files = db_get_namespace_files(s, nsid, &fc);
  for (uint32_t i = 0; i < fc; i++) {
    // Per-decl AST dep — see type_of_def.c. Editing a sibling decl
    // reproduces this fingerprint, so this query early-cuts.
    AstNodeId node = db_query_decl_ast(s, files[i], ast_id);
    if (node.idx == AST_NODE_ID_NONE.idx)
      continue;

    ASTStore *ast = db_get_file_ast(s, files[i]);
    AstNodeKind dk = ((AstNodeKind *)ast->kinds.data)[node.idx];
    if (dk != AST_DECL_CONST && dk != AST_DECL_VAR)
      return IP_NONE; // signature only defined on bind-decls

    AstNodeData d = ((AstNodeData *)ast->data.data)[node.idx];
    const uint32_t *ex = &((uint32_t *)ast->extra.data)[d.extra_idx.idx];
    AstNodeId value_id = {.idx = ex[2]};
    if (value_id.idx == AST_NODE_ID_NONE.idx)
      return IP_NONE;
    AstNodeKind vk = ((AstNodeKind *)ast->kinds.data)[value_id.idx];
    if (vk != AST_EXPR_LAMBDA)
      return IP_NONE; // signature only defined for fn-bound decls

    AstNodeData ld = ((AstNodeData *)ast->data.data)[value_id.idx];
    const uint32_t *lex = &((uint32_t *)ast->extra.data)[ld.extra_idx.idx];
    AstNodeId ret_node = {.idx = lex[0]};
    uint32_t param_count = lex[3];
    const uint32_t *param_ids = &lex[4];

    // Open a NodeTypeBuilder over the signature's AST sub-tree (each
    // param + the return-type-expr). sema_build_fn_type's recursive
    // sema_resolve_type_expr calls push every visited type-expr node's
    // resolved type into this builder. On completion the assembled
    // NodeTypesRange lands on db.fns.signature_node_types[fn_row]. The
    // body is intentionally NOT covered here — db_query_infer_body
    // owns the body's range.
    uint32_t sig_min = UINT32_MAX, sig_max = 0;
    for (uint32_t p = 0; p < param_count; p++) {
      AstNodeId pid = {.idx = param_ids[p]};
      if (pid.idx == AST_NODE_ID_NONE.idx)
        continue;
      uint32_t pmin = 0, pmax = 0;
      sema_ast_subtree_range(ast, pid, &pmin, &pmax);
      if (pmin < sig_min)
        sig_min = pmin;
      if (pmax > sig_max)
        sig_max = pmax;
    }
    if (ret_node.idx != AST_NODE_ID_NONE.idx) {
      uint32_t rmin = 0, rmax = 0;
      sema_ast_subtree_range(ast, ret_node, &rmin, &rmax);
      if (rmin < sig_min)
        sig_min = rmin;
      if (rmax > sig_max)
        sig_max = rmax;
    }
    if (sig_min == UINT32_MAX) {
      sig_min = 0;
      sig_max = 0;
    } // empty sig

    NodeTypeBuilder sig_b;
    sema_node_type_builder_begin(s, &sig_b, files[i], sig_min, sig_max);
    SemaCtx sig_ctx = {
        .s = s,
        .ast = ast,
        .nsid = nsid,
        .enclosing_fn = def,
        .file_local = files[i],
        .types = &sig_b,
    };

    IpIndex fn_ty =
        sema_build_fn_type(&sig_ctx, ret_node, param_ids, param_count);

    NodeTypesRange sig_range = sema_node_type_builder_end(&sig_b, NULL);

    // Stash the range on db.fns.signature_node_types[fn_row]. Same
    // idempotency as the body range — re-runs leak the previous
    // range's pool slots but the new range is canonical.
    if (db_def_kind(s, def) == KIND_FUNCTION) {
      uint32_t row = db_def_row(s, def, KIND_FUNCTION);
      *(NodeTypesRange *)vec_get(&s->fns.signature_node_types, row) = sig_range;
    }

    return fn_ty;
  }

  return IP_NONE;
}
