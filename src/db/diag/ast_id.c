// Phase P — body-relative AstId infrastructure. Lifecycle helpers for
// BodyAstIdMap. Building (preorder walk) lives in body_scopes.c; this
// file owns the init / free contract + the SyntaxNode → RelAstId lookup
// sema uses at emit time.

#include "ast_id.h"

#include "../../support/data_structure/hashmap.h"
#include "../../syntax/syntax.h"

void body_ast_id_map_init(BodyAstIdMap *m) {
  if (!m)
    return;
  hashmap_init(&m->rev);
  m->next_id = 0;
}

void body_ast_id_map_free(BodyAstIdMap *m) {
  if (!m)
    return;
  hashmap_free(&m->rev);
  m->next_id = 0;
}

// SyntaxNode → RelAstId lookup. Hash the node's ptr (same recipe the
// builder used in body_scopes.c), probe rev, decode the +1 sentinel
// used at insert time. Miss returns false; caller falls back to a
// FILE_RAW anchor (e.g. a node from a sub-query that walked outside
// the body subtree).
bool body_ast_id_lookup(const BodyAstIdMap *m, const SyntaxNode *node,
                        uint32_t *out_rel) {
  if (!m || !node || !out_rel)
    return false;
  uint64_t h = syntax_node_ptr_hash(syntax_node_ptr_new(node));
  void *v = hashmap_get(&m->rev, h);
  if (!v)
    return false;
  *out_rel = (uint32_t)((uintptr_t)v - 1);
  return true;
}
