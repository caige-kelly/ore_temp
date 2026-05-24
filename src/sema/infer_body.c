#include "../db/db.h"
#include "../db/intern_pool/intern_pool.h"
#include "../db/query/body_scopes.h"
#include "../db/query/decl_ast.h"
#include "../db/query/def_identity.h"
#include "../db/query/fn_signature.h"
#include "../db/query/index.h"
#include "../db/query/query_engine.h"
#include "../parser/ast.h"
#include "sema.h"

// Body type inference — chunk 5d/5i. With body_scopes now its own
// query, this function's job is just: (1) declare a dep on
// body_scopes(def) so its result is available to type_of_expr's path
// lookups, (2) bidirectional-check the body against the declared
// return type, (3) accumulate the typed-body fingerprint (Phase 7)
// if the caller passed a non-NULL out param.
//
// All bind collection moved to sema/body_scopes.c.
IpIndex sema_infer_body(struct db *s, DefId def, Fingerprint *body_fp_out) {
  AstId ast_id = *(AstId *)vec_get(&s->defs.ast_ids, def.idx);
  NamespaceId nsid = *(NamespaceId *)vec_get(&s->defs.parent_modules, def.idx);

  // Depend on the module's top-level index — this body reads the
  // module's file list below; a file-set change must re-run it.
  (void)db_query_top_level_index(s, nsid);
  (void)db_query_def_identity(s, nsid, ast_id);
  IpIndex sig = db_query_fn_signature(s, def);
  if (sig.v == IP_NONE.v)
    return IP_NONE;

  // Records dep on body_scopes(def) — any AST edit that reshapes the
  // body's binding structure (or changes a referenced decl's type)
  // invalidates this query. type_of_expr's PATH handler reads the
  // body-scope pools raw under the umbrella of this dep.
  (void)db_query_body_scopes(s, def);

  // Locate the lambda + body node to drive the return-type check.
  ASTStore *ast = NULL;
  AstNodeId lambda_node = AST_NODE_ID_NONE;
  FileId body_fid = FILE_ID_NONE;
  uint32_t fc = 0;
  const FileId *files = db_get_namespace_files(s, nsid, &fc);
  for (uint32_t i = 0; i < fc; i++) {
    // Per-decl AST dep — see type_of_def.c.
    AstNodeId node = db_query_decl_ast(s, files[i], ast_id);
    if (node.idx == AST_NODE_ID_NONE.idx)
      continue;

    ASTStore *cand_ast = db_get_file_ast(s, files[i]);
    AstNodeKind dk = ((AstNodeKind *)cand_ast->kinds.data)[node.idx];
    if (dk != AST_DECL_CONST && dk != AST_DECL_VAR)
      break;

    AstNodeData d = ((AstNodeData *)cand_ast->data.data)[node.idx];
    const uint32_t *ex = &((uint32_t *)cand_ast->extra.data)[d.extra_idx.idx];
    AstNodeId value_id = {.idx = ex[2]};
    if (value_id.idx == AST_NODE_ID_NONE.idx)
      break;
    AstNodeKind vk = ((AstNodeKind *)cand_ast->kinds.data)[value_id.idx];
    if (vk != AST_EXPR_LAMBDA)
      break;

    ast = cand_ast;
    lambda_node = value_id;
    body_fid = files[i];
    break;
  }

  if (!ast || lambda_node.idx == AST_NODE_ID_NONE.idx)
    return sig;

  // Lambda extras: [ret_id, body_id, effect_id, param_count, p0, ...].
  AstNodeData ld = ((AstNodeData *)ast->data.data)[lambda_node.idx];
  const uint32_t *lex = &((uint32_t *)ast->extra.data)[ld.extra_idx.idx];
  AstNodeId body_node = {.idx = lex[1]};

  // Open a NodeTypeBuilder over the body's AST sub-tree. sema_check_expr
  // and its recursive sema_type_of_expr calls push every visited node's
  // resolved type into this builder; on completion the assembled
  // NodeTypesRange lands on db.fns.body_node_types[fn_row] for the
  // unified node_type router to read. Mirrors RA's per-body
  // InferenceResult — the query's body owns the range, no side-effect
  // walker repopulation.
  NodeTypeBuilder body_b;
  uint32_t b_min = 0, b_max = 0;
  if (body_node.idx != AST_NODE_ID_NONE.idx)
    sema_ast_subtree_range(ast, body_node, &b_min, &b_max);
  sema_node_type_builder_begin(s, &body_b, body_fid, b_min, b_max);

  if (body_node.idx != AST_NODE_ID_NONE.idx) {
    IpKey sig_key = ip_key(&s->intern, sig);
    IpIndex expected_ret = sig_key.fn_type.ret;
    (void)sema_check_expr(s, ast, body_node, expected_ret, nsid, def, body_fid);
  }

  Fingerprint body_range_fp = 0;
  NodeTypesRange body_range =
      sema_node_type_builder_end(s, &body_b, &body_range_fp);

  // Stash the range on db.fns.body_node_types[fn_row] so the router
  // (db_query_node_type) can read it without re-running this query.
  // Idempotent on re-runs: the previous run's range still occupies
  // its pool slot but becomes unreachable; this run's NEW range
  // is the canonical one.
  if (db_def_kind(s, def) == KIND_FUNCTION) {
    uint32_t row = db_def_row(s, def, KIND_FUNCTION);
    *(NodeTypesRange *)vec_get(&s->fns.body_node_types, row) = body_range;
  }

  // Phase 7 — typed-body fingerprint. The NodeTypeBuilder accumulated
  // a (node.idx, type.v) fold over every body node it visited as the
  // body was being type-checked; we receive it from builder_end as
  // body_range_fp. Equivalent to the old TinySpan-gated post-walk over
  // FileNodeData.types[i], but stable under pool-offset shifts and
  // sourced from the same data the IDE router reads (single source of
  // truth, no separate sweep).
  if (body_fp_out)
    *body_fp_out = body_range_fp;

  return sig;
}
