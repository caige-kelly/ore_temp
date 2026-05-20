#include "../db/db.h"
#include "../db/intern_pool/intern_pool.h"
#include "../db/query/ast.h"
#include "../db/query/def_identity.h"
#include "../db/query/fn_signature.h"
#include "../db/storage/hashmap.h"
#include "../db/workspace/ast_id_map.h"
#include "../parser/ast.h"
#include "sema.h"

// Encoding for HashMap value slots: we want NULL to mean "absent",
// so each stored IpIndex.v gets shifted up by 1. IP_NONE (UINT32_MAX)
// is never stored — we filter it out before insert.
static inline void *encode_ip(IpIndex t) {
  return (void *)(uintptr_t)((uint64_t)t.v + 1u);
}
static inline IpIndex decode_ip(void *p) {
  return (IpIndex){.v = (uint32_t)((uintptr_t)p - 1u)};
}

IpIndex sema_local_scope_lookup(struct db *s, DefId enclosing_fn,
                                StrId name) {
  if (enclosing_fn.idx == DEF_ID_NONE.idx)
    return IP_NONE;
  if (enclosing_fn.idx >= s->defs.local_scopes.count)
    return IP_NONE;
  HashMap *scope =
      *(HashMap **)vec_get(&s->defs.local_scopes, enclosing_fn.idx);
  if (!scope)
    return IP_NONE;
  void *p = hashmap_get(scope, (uint64_t)name.idx);
  if (!p)
    return IP_NONE;
  return decode_ip(p);
}

IpIndex sema_infer_body(struct db *s, DefId def) {
  AstId ast_id = *(AstId *)vec_get(&s->defs.ast_ids, def.idx);
  ModuleId mid = *(ModuleId *)vec_get(&s->defs.parent_modules, def.idx);

  (void)db_query_def_identity(s, mid, ast_id);
  IpIndex sig = db_query_fn_signature(s, def);
  if (sig.v == IP_NONE.v)
    return IP_NONE;

  // Locate the lambda AST node. Each db_query_file_ast records the dep.
  ASTStore *ast = NULL;
  AstNodeId lambda_node = AST_NODE_ID_NONE;
  uint32_t fc = 0;
  const FileId *files = db_module_files(s, mid, &fc);
  for (uint32_t i = 0; i < fc; i++) {
    (void)db_query_file_ast(s, files[i]);
    uint32_t local = file_id_local(files[i]);
    struct AstIdMap *map =
        *(struct AstIdMap **)vec_get(&s->files.ast_id_maps, local);
    if (!map)
      continue;
    AstNodeId node = ast_id_map_get(map, ast_id);
    if (node.idx == AST_NODE_ID_NONE.idx)
      continue;

    ASTStore *cand_ast = *(ASTStore **)vec_get(&s->files.asts, local);
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
    break;
  }

  if (!ast || lambda_node.idx == AST_NODE_ID_NONE.idx)
    return IP_NONE;

  // Allocate or reuse the per-fn local-scope HashMap. The map lives in
  // db.arena; hashmap_clear preserves the backing storage so re-runs
  // don't churn allocations.
  HashMap **scope_slot =
      (HashMap **)vec_get(&s->defs.local_scopes, def.idx);
  if (!*scope_slot)
    *scope_slot = hashmap_new_in(&s->arena);
  else
    hashmap_clear(*scope_slot);
  HashMap *scope = *scope_slot;

  // Recover param types from the interned fn signature (structural —
  // IpKey hands us the exact param IpIndex array). Names come from
  // the AST since they don't participate in type identity.
  IpKey sig_key = ip_key(&s->intern, sig);
  const IpIndex *sig_params = sig_key.fn_type.params;
  size_t n_sig_params = sig_key.fn_type.n_params;

  // Lambda extras: [ret_id, body_id, effect_id, param_count, p0, ...].
  AstNodeData ld = ((AstNodeData *)ast->data.data)[lambda_node.idx];
  const uint32_t *lex = &((uint32_t *)ast->extra.data)[ld.extra_idx.idx];
  uint32_t n_ast_params = lex[3];
  const uint32_t *param_ids = &lex[4];

  uint32_t n = (uint32_t)((n_ast_params < n_sig_params) ? n_ast_params
                                                        : n_sig_params);
  for (uint32_t i = 0; i < n; i++) {
    AstNodeId pid = {.idx = param_ids[i]};
    if (pid.idx == AST_NODE_ID_NONE.idx)
      continue;
    AstNodeKind pk = ((AstNodeKind *)ast->kinds.data)[pid.idx];
    if (pk != AST_DECL_PARAM)
      continue;
    AstNodeData pd = ((AstNodeData *)ast->data.data)[pid.idx];
    const uint32_t *pex = &((uint32_t *)ast->extra.data)[pd.extra_idx.idx];
    StrId pname = sema_decl_name_from_node(ast, pex[0]);
    if (pname.idx == 0)
      continue;

    IpIndex pty = sig_params[i];
    if (pty.v == IP_NONE.v)
      continue;

    hashmap_put_or_die(scope, (uint64_t)pname.idx, encode_ip(pty),
                       "local_scope.param");
  }

  return sig;
}
