#include "decl_ast.h"

#include "../../parser/ast.h"
#include "../db.h"
#include "../workspace/ast_id_map.h"
#include "ast.h"
#include "query.h"

// db_query_decl_ast — per-decl AST handle. See decl_ast.h.
//
// Routed-SoA, mirroring db_query_def_identity: the (file, ast_id) key is
// packed and mapped to a dense row via db.decl_ast_cache; the row is
// allocated once and never moves, so the slot is stable. The query's
// value (results[row]) is the decl's current AstNodeId; its fingerprint
// is the structural subtree hash, which is what makes a sibling edit
// early-cut this decl's sema consumers.
AstNodeId db_query_decl_ast(struct db *s, FileId fid, AstId ast_id) {
  if (fid.idx == FILE_ID_NONE.idx || ast_id.idx == AST_ID_NONE.idx)
    return AST_NODE_ID_NONE;

  // Store the FULL FileId.idx (incl. the virtual bit) in the high 32:
  // db_force_query reconstructs `fid` from this key when forcing a
  // recompute of this slot, and stripping the virtual bit would lose
  // physical-vs-virtual identity for the reconstructed FileId.
  uint64_t k = ((uint64_t)fid.idx << 32) | (uint64_t)ast_id.idx;

  // Route (file, ast_id) → dense row in db.decl_ast (allocate on first
  // sight; row 0 is the reserved sentinel).
  void *rowp = hashmap_get(&s->decl_ast_cache, k);
  uint32_t row;
  if (!rowp) {
    row = (uint32_t)s->decl_ast.slots_hot.count;
    vec_push_zero(&s->decl_ast.results);
    vec_push_zero(&s->decl_ast.slots_hot);
    vec_push_zero(&s->decl_ast.slots_cold);
    hashmap_put_or_die(&s->decl_ast_cache, k, (void *)(uintptr_t)row,
                       "decl_ast_cache");
  } else {
    row = (uint32_t)(uintptr_t)rowp;
  }

  DB_QUERY_GUARD(s, QUERY_DECL_AST, k,
                 *(AstNodeId *)vec_get(&s->decl_ast.results, row),
                 AST_NODE_ID_NONE, AST_NODE_ID_NONE);

  // Dep on the whole-file parse (auto-recorded onto this query's frame).
  // Editing the file stales QUERY_FILE_AST → this query re-runs → the
  // structural fingerprint below is recomputed.
  db_query_file_ast(s, fid);

  // Resolve ast_id → the decl's current AstNodeId for this file.
  struct AstIdMap *map = db_get_file_ast_id_map(s, fid);
  AstNodeId node = map ? ast_id_map_get(map, ast_id) : AST_NODE_ID_NONE;
  if (node.idx == AST_NODE_ID_NONE.idx) {
    // The decl is not in this file — a deterministic failure (every
    // module file is queried; only one owns the decl). FINGERPRINT_NONE
    // is stable, so a consumer's dep on this slot stays early-cuttable.
    db_query_fail(s, QUERY_DECL_AST, k);
    return AST_NODE_ID_NONE;
  }

  struct ASTStore *ast = db_get_file_ast(s, fid);
  Fingerprint fp = ast ? ast_subtree_fingerprint(ast, node) : FINGERPRINT_NONE;

  *(AstNodeId *)vec_get(&s->decl_ast.results, row) = node;
  db_query_succeed(s, QUERY_DECL_AST, k, fp);
  return node;
}
