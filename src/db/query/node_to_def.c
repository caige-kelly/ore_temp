#include "node_to_def.h"

#include "../db.h"
#include "ast.h"
#include "def_identity.h"
#include "query.h"
#include "query_engine.h"

// db_query_node_to_def — stamp the file's node→DefId reverse index.
//
// node→ast_id is pure AST (the file's top_level_index); ast_id→DefId is
// db_query_def_identity. So this query composes those two: it deps on
// the file's parse and on each top-level decl's identity, and writes
// ModuleNodeData.defs (reclaimed + rebuilt on every reparse).
bool db_query_node_to_def(struct db *s, FileId fid) {
  if (fid.idx == FILE_ID_NONE.idx)
    return false;

  DB_QUERY_GUARD(s, QUERY_NODE_TO_DECL, (uint64_t)fid.idx, true, false, false);

  // Parse the file (records the QUERY_FILE_AST dep) and resolve its
  // owning module — db_query_def_identity is keyed by (module, ast_id).
  db_query_file_ast(s, fid);
  uint32_t local = file_id_local(fid);
  ModuleId mid = db_get_file_module(s, fid);

  ModuleNodeData *nd = (ModuleNodeData *)vec_get(&s->files.node_data, local);
  FileArray *idx = (FileArray *)vec_get(&s->files.top_level_indices, local);

  Fingerprint fp = db_fp_u64(idx ? idx->count : 0);
  if (idx && nd && nd->defs) {
    for (size_t i = 0; i < idx->count; i++) {
      TopLevelEntry *e = &((TopLevelEntry *)idx->data)[i];
      if (e->node.idx == AST_NODE_ID_NONE.idx)
        continue;
      // db_query_def_identity records the dep on the decl's identity
      // slot, so a kind/name change to the decl re-runs this query.
      DefId def = db_query_def_identity(s, mid, e->ast_id);
      nd->defs[e->node.idx] = def;
      fp = db_fp_combine(fp, db_fp_u64((uint64_t)e->node.idx));
      fp = db_fp_combine(fp, db_fp_u64((uint64_t)def.idx));
    }
  }

  db_query_succeed(s, QUERY_NODE_TO_DECL, (uint64_t)fid.idx, fp);
  return true;
}
