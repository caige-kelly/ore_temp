#include "../db/db.h"
#include "../db/intern_pool/intern_pool.h"
#include "../db/query/ast.h"
#include "../db/query/body_scopes.h"
#include "../db/query/def_identity.h"
#include "../db/query/fn_signature.h"
#include "../db/query/index.h"
#include "../db/workspace/ast_id_map.h"
#include "../parser/ast.h"
#include "sema.h"

// Body type inference — chunk 5d/5i. With body_scopes now its own
// query, this function's job is just: (1) declare a dep on
// body_scopes(def) so its result is available to type_of_expr's path
// lookups, then (2) bidirectional-check the body against the declared
// return type. All bind collection moved to sema/body_scopes.c.
IpIndex sema_infer_body(struct db *s, DefId def) {
  AstId ast_id = *(AstId *)vec_get(&s->defs.ast_ids, def.idx);
  ModuleId mid = *(ModuleId *)vec_get(&s->defs.parent_modules, def.idx);

  // Depend on the module's top-level index — this body reads the
  // module's file list below; a file-set change must re-run it.
  (void)db_query_top_level_index(s, mid);
  (void)db_query_def_identity(s, mid, ast_id);
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
  const FileId *files = db_get_module_files(s, mid, &fc);
  for (uint32_t i = 0; i < fc; i++) {
    (void)db_query_file_ast(s, files[i]);
    struct AstIdMap *map = db_get_file_ast_id_map(s, files[i]);
    if (!map)
      continue;
    AstNodeId node = ast_id_map_get(map, ast_id);
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

  if (body_node.idx != AST_NODE_ID_NONE.idx) {
    IpKey sig_key = ip_key(&s->intern, sig);
    IpIndex expected_ret = sig_key.fn_type.ret;
    (void)sema_check_expr(s, ast, body_node, expected_ret, mid, def, body_fid);
  }

  return sig;
}
