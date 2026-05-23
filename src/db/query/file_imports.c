// QUERY_FILE_IMPORTS — walk a file's AST for @import refs. Pure.

#include "file_imports.h"

#include "../../parser/ast.h"
#include "../db.h"
#include "../storage/arena.h"
#include "ast.h"
#include "query.h"
#include "query_engine.h"

#include <string.h>

FileArray *db_query_file_imports(struct db *s, FileId fid) {
  if (fid.idx == FILE_ID_NONE.idx)
    return NULL;
  uint32_t f = file_id_local(fid);
  if (f >= s->files.imports.count)
    return NULL;
  FileArray *out = (FileArray *)vec_get(&s->files.imports, f);

  DB_QUERY_GUARD(s, QUERY_FILE_IMPORTS, (uint64_t)fid.idx, out, NULL, NULL);

  // Dep on the whole-file parse (auto-recorded). File text change →
  // file_ast recomputes → file_imports invalidates → re-walks.
  (void)db_query_file_ast(s, fid);

  ASTStore *ast = db_get_file_ast(s, fid);
  if (!ast) {
    out->data = NULL;
    out->count = 0;
    db_query_succeed(s, QUERY_FILE_IMPORTS, (uint64_t)fid.idx, FINGERPRINT_NONE);
    return out;
  }

  // Pass 1: count import refs (BUILTIN whose name is the pre-interned
  // "import" StrId and whose first arg is a string literal).
  AstNodeKind *kinds = (AstNodeKind *)ast->kinds.data;
  AstNodeData *data = (AstNodeData *)ast->data.data;
  uint32_t   *extra = (uint32_t *)ast->extra.data;
  size_t n_nodes = ast->kinds.count;
  StrId import_name = s->names.IMPORT;

  uint32_t count = 0;
  for (size_t i = 1; i < n_nodes; i++) { // skip row 0 (sentinel)
    if (kinds[i] != AST_EXPR_BUILTIN)
      continue;
    AstExtraDataIdx ex = data[i].extra_idx;
    uint32_t *e = &extra[ex.idx];
    StrId nm = {.idx = e[0]};
    if (nm.idx != import_name.idx)
      continue;
    uint32_t arg_count = e[1];
    if (arg_count < 1)
      continue;
    AstNodeId arg0 = {.idx = e[2]};
    if (arg0.idx == AST_NODE_ID_NONE.idx || arg0.idx >= n_nodes)
      continue;
    if (kinds[arg0.idx] != AST_EXPR_LIT_STRING)
      continue;
    count++;
  }

  // Allocate in the file's arena; pointer is valid until the next
  // QUERY_FILE_AST reparse (which resets this arena).
  Arena *fa = (Arena *)vec_get(&s->files.arenas, f);
  ImportRef *refs = NULL;
  if (count > 0)
    refs = (ImportRef *)arena_alloc_raw(fa, (size_t)count * sizeof(ImportRef));

  // Pass 2: collect.
  uint32_t j = 0;
  Fingerprint fp = (Fingerprint)count;
  for (size_t i = 1; i < n_nodes && j < count; i++) {
    if (kinds[i] != AST_EXPR_BUILTIN)
      continue;
    AstExtraDataIdx ex = data[i].extra_idx;
    uint32_t *e = &extra[ex.idx];
    StrId nm = {.idx = e[0]};
    if (nm.idx != import_name.idx)
      continue;
    uint32_t arg_count = e[1];
    if (arg_count < 1)
      continue;
    AstNodeId arg0 = {.idx = e[2]};
    if (arg0.idx == AST_NODE_ID_NONE.idx || arg0.idx >= n_nodes ||
        kinds[arg0.idx] != AST_EXPR_LIT_STRING)
      continue;
    refs[j].path = data[arg0.idx].string_id;
    refs[j].site = (AstNodeId){.idx = (uint32_t)i};
    fp = db_fp_combine(fp, db_fp_u64((uint64_t)refs[j].path.idx));
    j++;
  }

  out->data = refs;
  out->count = count;
  db_query_succeed(s, QUERY_FILE_IMPORTS, (uint64_t)fid.idx, fp);
  return out;
}
