#include "node_to_def.h"

#include "../db.h"
#include "../../syntax/syntax.h"
#include "ast.h"
#include "def_identity.h"
#include "query.h"
#include "query_engine.h"

// db_query_node_to_def — stamp the file's sparse top-level-only
// node→DefId reverse index (files.node_to_def HashMap).
//
// node_ptr → DefId is db_query_def_identity. Only top-level decl
// wrappers go into the map; db_get_def_for_node climbs syntax parents
// to reach a top-level wrapper before probing.
bool db_query_node_to_def(struct db *s, FileId fid) {
  if (fid.idx == FILE_ID_NONE.idx)
    return false;

  DB_QUERY_GUARD(s, QUERY_NODE_TO_DECL, (uint64_t)fid.idx, true, false, false);

  // Parse the file (records the QUERY_FILE_AST dep) and resolve its
  // owning module — db_query_def_identity is keyed by (module, node_ptr).
  db_query_file_ast(s, fid);
  uint32_t local = file_id_local(fid);
  NamespaceId nsid = db_get_file_namespace(s, fid);

  HashMap *map = (HashMap *)vec_get(&s->files.node_to_def, local);
  hashmap_clear(map);
  FileArray *idx = (FileArray *)vec_get(&s->files.top_level_indices, local);

  Fingerprint fp = db_fp_u64(idx ? idx->count : 0);
  if (idx) {
    for (size_t i = 0; i < idx->count; i++) {
      TopLevelEntry *e = &((TopLevelEntry *)idx->data)[i];
      // db_query_def_identity records the dep on the decl's identity
      // slot, so a kind/name change to the decl re-runs this query.
      DefId def = db_query_def_identity(s, nsid, e->node_ptr);
      uint64_t key = syntax_node_ptr_hash(e->node_ptr);
      hashmap_put(map, key, (void *)(uintptr_t)def.idx);
      fp = db_fp_combine(fp, db_fp_u64(key));
      fp = db_fp_combine(fp, db_fp_u64((uint64_t)def.idx));
    }
  }

  db_query_succeed(s, QUERY_NODE_TO_DECL, (uint64_t)fid.idx, fp);
  return true;
}
