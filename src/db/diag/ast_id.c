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
