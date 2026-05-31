// Phase P P1 — AstId infrastructure. Lifecycle helpers for
// FileAstIdMap and BodyAstIdMap. Building (preorder walk) lives in
// the respective query bodies (parse.c, scope.c); this file owns the
// init / free contract.

#include "ast_id.h"

#include "../../support/data_structure/hashmap.h"
#include "../../support/data_structure/vec.h"
#include "../../syntax/syntax.h"

void file_ast_id_map_init(FileAstIdMap *m) {
  if (!m)
    return;
  vec_init(&m->ptrs, sizeof(SyntaxNodePtr));
  hashmap_init(&m->rev);
}

void file_ast_id_map_free(FileAstIdMap *m) {
  if (!m)
    return;
  vec_free(&m->ptrs);
  hashmap_free(&m->rev);
}

void body_ast_id_map_init(BodyAstIdMap *m) {
  if (!m)
    return;
  vec_init(&m->ptrs, sizeof(SyntaxNodePtr));
  hashmap_init(&m->rev);
}

void body_ast_id_map_free(BodyAstIdMap *m) {
  if (!m)
    return;
  vec_free(&m->ptrs);
  hashmap_free(&m->rev);
}

// Phase P S4 — SyntaxNode → id lookups. Hash the node's ptr (same
// recipe the builder used in body_scopes.c / parse.c), probe the
// `rev` HashMap, decode the +1 sentinel used at insert time. Miss
// returns false; caller falls back to a non-AstId anchor.
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

bool file_ast_id_lookup(const FileAstIdMap *m, const SyntaxNode *node,
                        uint32_t *out_id) {
  if (!m || !node || !out_id)
    return false;
  uint64_t h = syntax_node_ptr_hash(syntax_node_ptr_new(node));
  void *v = hashmap_get(&m->rev, h);
  if (!v)
    return false;
  *out_id = (uint32_t)((uintptr_t)v - 1);
  return true;
}
