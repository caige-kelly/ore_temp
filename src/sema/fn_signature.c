#include "../ast/ast_decl.h"
#include "../ast/ast_expr.h"
#include "../db/db.h"
#include "../db/intern_pool/intern_pool.h"
#include "../db/query/decl_ast.h"
#include "../db/query/def_identity.h"
#include "../db/query/index.h"
#include "../parser/syntax_kind.h"
#include "../syntax/syntax.h"
#include "sema.h"

// Build an interned fn type from a param-list SyntaxNode + return-type
// SyntaxNode. Shared by the lambda-RHS path (sema_fn_signature) and
// the type-position path (sema_resolve_type_expr's SK_FN_TYPE case).
// Identity is purely structural — (ret, modifiers, params) — so an
// anonymous fn type in a field dedups with a top-level fn that shares
// its shape.
//
// Scratch params array comes from db.request_arena (reset at
// db_request_end), so n_params has no compile-time cap.
//
// `param_list` is the SK_PARAM_LIST wrapper; NULL means zero params.
// `ret_node` is the optional return-type node; NULL means implicit void.
IpIndex sema_build_fn_type(const SemaCtx *ctx, SyntaxNode *ret_node,
                           SyntaxNode *param_list) {
  struct db *s = ctx->s;

  // Count Params first so the scratch buffer is sized exactly.
  uint32_t n_params = 0;
  if (param_list) {
    uint32_t pc = syntax_node_num_children(param_list);
    for (uint32_t i = 0; i < pc; i++) {
      GreenElement g = green_node_child(syntax_node_green(param_list), i);
      if (g.kind == GREEN_ELEM_NODE &&
          green_node_kind(g.node) == SK_PARAM)
        n_params++;
    }
  }

  IpIndex *params = NULL;
  if (n_params > 0) {
    params = arena_alloc(&s->request_arena, n_params * sizeof(IpIndex));
    if (!params)
      return IP_NONE;
  }

  // Walk Param wrappers, resolve each Param_type, and stamp the param
  // node itself so hover on the param-name token reads its type back.
  if (param_list) {
    uint32_t pc = syntax_node_num_children(param_list);
    uint32_t out = 0;
    for (uint32_t i = 0; i < pc; i++) {
      SyntaxElement el = syntax_node_child_or_token(param_list, i);
      if (el.kind != SYNTAX_ELEM_NODE || !el.node) {
        if (el.kind == SYNTAX_ELEM_TOKEN && el.token)
          syntax_token_release(el.token);
        continue;
      }
      Param p;
      if (!Param_cast(el.node, &p)) {
        syntax_node_release(el.node);
        continue;
      }
      SyntaxNode *ptype = Param_type(&p);
      IpIndex pti = ptype ? sema_resolve_type_expr(ctx, ptype) : IP_NONE;
      if (ptype) syntax_node_release(ptype);
      if (pti.v == IP_NONE.v) {
        syntax_node_release(el.node);
        return IP_NONE;
      }
      params[out++] = pti;
      sema_node_type_builder_push(ctx, el.node, pti);
      syntax_node_release(el.node);
    }
  }

  IpIndex ret;
  if (!ret_node) {
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
               // params is borrowed from request_arena — stamp it so ip_get
               // can assert the key is consumed before the next request reset.
               .src_arena = params ? &s->request_arena : NULL,
               .src_gen = s->request_arena.generation};
  return ip_get(&s->intern, key);
}

IpIndex sema_fn_signature(struct db *s, DefId def) {
  SyntaxNodePtr def_ptr = *(SyntaxNodePtr *)vec_get(&s->defs.syntax_ptrs, def.idx);
  NamespaceId nsid = *(NamespaceId *)vec_get(&s->defs.parent_modules, def.idx);

  // Depend on the module's top-level index — this body reads the
  // module's file list below; a file-set change must re-run it.
  (void)db_query_top_level_index(s, nsid);
  (void)db_query_def_identity(s, nsid, def_ptr);

  uint32_t fc = 0;
  const FileId *files = db_get_namespace_files(s, nsid, &fc);
  for (uint32_t i = 0; i < fc; i++) {
    // Per-decl AST dep — see type_of_def.c. Editing a sibling decl
    // reproduces this fingerprint, so this query early-cuts.
    SyntaxNodePtr ptr = db_query_decl_ast(s, files[i], def_ptr);
    if (ptr.kind == SYNTAX_KIND_NONE)
      continue;

    // Resolve the wrapper SyntaxNode against this file's current tree.
    uint32_t local = file_id_local(files[i]);
    GreenNode *groot = *(GreenNode **)vec_get(&s->files.green_roots, local);
    if (!groot)
      continue;
    SyntaxTree *tree = syntax_tree_new(groot);
    SyntaxNode *root_red = syntax_tree_root(tree);
    SyntaxNode *wrapper = syntax_node_ptr_resolve(ptr, root_red);
    syntax_node_release(root_red);

    if (!wrapper) {
      syntax_tree_free(tree);
      continue;
    }

    // The signature exists only on CONST_DECL / VAR_DECL bound to a
    // lambda. Anything else is a non-fn def for which this query is
    // undefined.
    SyntaxNode *value = NULL;
    SyntaxKind wk = syntax_node_kind(wrapper);
    if (wk == SK_CONST_DECL) {
      ConstDef c;
      if (ConstDef_cast(wrapper, &c)) value = ConstDef_value(&c);
    } else if (wk == SK_VAR_DECL) {
      VarDef v;
      if (VarDef_cast(wrapper, &v)) value = VarDef_value(&v);
    }
    syntax_node_release(wrapper);
    if (!value) {
      syntax_tree_free(tree);
      return IP_NONE;
    }

    LambdaExpr lam;
    if (!LambdaExpr_cast(value, &lam)) {
      syntax_node_release(value);
      syntax_tree_free(tree);
      return IP_NONE;
    }
    SyntaxNode *params = LambdaExpr_params(&lam);
    SyntaxNode *ret_node = LambdaExpr_return_type(&lam);

    // Open a NodeTypeBuilder. Phase 4: HashMap-backed, no
    // min/max sizing.
    NodeTypeBuilder sig_b;
    sema_node_type_builder_begin(s, &sig_b, files[i]);
    SemaCtx sig_ctx = {
        .s = s,
        .file_green_root = groot,
        .nsid = nsid,
        .enclosing_fn = def,
        .file_local = files[i],
        .types = &sig_b,
    };

    IpIndex fn_ty = sema_build_fn_type(&sig_ctx, ret_node, params);

    NodeTypesRange sig_range = sema_node_type_builder_end(&sig_b, NULL);

    // Stash the range on db.fns.signature_node_types[fn_row]. Same
    // idempotency as the body range — re-runs leak the previous
    // range's pool slots but the new range is canonical.
    if (db_def_kind(s, def) == KIND_FUNCTION) {
      uint32_t row = db_def_row(s, def, KIND_FUNCTION);
      *(NodeTypesRange *)vec_get(&s->fns.signature_node_types, row) = sig_range;
    }

    if (params) syntax_node_release(params);
    if (ret_node) syntax_node_release(ret_node);
    syntax_node_release(value);
    syntax_tree_free(tree);
    return fn_ty;
  }

  return IP_NONE;
}
