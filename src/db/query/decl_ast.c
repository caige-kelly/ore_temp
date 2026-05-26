#include "decl_ast.h"

#include "../db.h"
#include "../../syntax/syntax.h"
#include "ast.h"
#include "query.h"

// db_query_decl_ast — per-decl green-tree handle.
//
// Routed-SoA, mirroring db_query_def_identity: the (file, ptr-hash)
// key is packed and mapped to a dense row via db.decl_ast_cache; the
// row is allocated once and never moves, so the slot is stable. The
// query's value (results[row]) is the decl's current SyntaxNodePtr;
// its fingerprint is the trivia-stripped structural subtree hash
// (Phase 4.4), which is what makes a sibling-decl edit early-cut
// this decl's sema consumers.
SyntaxNodePtr db_query_decl_ast(struct db *s, FileId fid,
                                SyntaxNodePtr node_ptr) {
  SyntaxNodePtr none = {0};
  if (fid.idx == FILE_ID_NONE.idx)
    return none;

  // Store the FULL FileId.idx (incl. the virtual bit) in the high 32:
  // db_force_query reconstructs `fid` from this key when forcing a
  // recompute of this slot, and stripping the virtual bit would lose
  // physical-vs-virtual identity for the reconstructed FileId.
  uint64_t k =
      ((uint64_t)fid.idx << 32) | (uint32_t)syntax_node_ptr_hash(node_ptr);

  // Route (file, ptr-hash) → dense row in db.decl_ast (allocate on
  // first sight; row 0 is the reserved sentinel).
  void *rowp = hashmap_get(&s->decl_ast_cache, k);
  uint32_t row;
  if (!rowp) {
    row = (uint32_t)s->decl_ast.slots_hot.count;
    vec_push_zero(&s->decl_ast.results);
    vec_push_zero(&s->decl_ast.keys);
    vec_push_zero(&s->decl_ast.slots_hot);
    vec_push_zero(&s->decl_ast.slots_cold);
    // Stash the original SyntaxNodePtr so recompute_decl_ast can
    // recover it from the routing key.
    *(SyntaxNodePtr *)vec_get(&s->decl_ast.keys, row) = node_ptr;
    hashmap_put_or_die(&s->decl_ast_cache, k, (void *)(uintptr_t)row,
                       "decl_ast_cache");
  } else {
    row = (uint32_t)(uintptr_t)rowp;
  }

  DB_QUERY_GUARD(s, QUERY_DECL_AST, k,
                 *(SyntaxNodePtr *)vec_get(&s->decl_ast.results, row), none,
                 none);

  // Dep on the whole-file parse (auto-recorded onto this query's
  // frame). Editing the file stales QUERY_FILE_AST → this query
  // re-runs → the structural fingerprint below is recomputed.
  db_query_file_ast(s, fid);

  // Resolve node_ptr against the file's current green tree.
  uint32_t local = file_id_local(fid);
  if (local >= s->files.green_roots.count) {
    db_query_fail(s, QUERY_DECL_AST, k);
    return none;
  }
  GreenNode *groot = *(GreenNode **)vec_get(&s->files.green_roots, local);
  if (!groot) {
    db_query_fail(s, QUERY_DECL_AST, k);
    return none;
  }
  SyntaxTree *tree = syntax_tree_new(groot);
  SyntaxNode *root_red = syntax_tree_root(tree);
  SyntaxNode *resolved = syntax_node_ptr_resolve(node_ptr, root_red);

  if (!resolved) {
    // The decl is not in this file's current tree (deterministic
    // failure). FINGERPRINT_NONE is stable, so a consumer's dep on
    // this slot stays early-cuttable.
    syntax_node_release(root_red);
    syntax_tree_free(tree);
    db_query_fail(s, QUERY_DECL_AST, k);
    return none;
  }

  // Fingerprint: Phase 4.4 will replace this with a trivia-stripped
  // recursive hash. For now, fold the resolved subtree's text_len and
  // the green_node's cached content_hash so identical content
  // produces identical fingerprints across reparses where trivia is
  // unchanged.
  const GreenNode *gn = syntax_node_green(resolved);
  Fingerprint fp = db_fp_u64(green_node_text_len(gn));
  syntax_node_release(resolved);
  syntax_node_release(root_red);
  syntax_tree_free(tree);

  *(SyntaxNodePtr *)vec_get(&s->decl_ast.results, row) = node_ptr;
  db_query_succeed(s, QUERY_DECL_AST, k, fp);
  return node_ptr;
}
