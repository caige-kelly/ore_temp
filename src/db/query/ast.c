#include "ast.h"
#include "../../lexer/layout.h"
#include "../../lexer/lexer.h"
#include "../../parser/ast.h" // ASTStore (for fingerprinting)
#include "../../parser/parser.h"
#include "invalidate.h"

Fingerprint db_query_module_ast(struct db *s, ModuleId mod) {
  ModuleId *stable_mod = (ModuleId *)vec_get(&s->modules.ids, mod.idx);

  // DB_QUERY_GUARD evaluates the on_cached expression only when the
  // begin returns CACHED; we re-locate inside that branch rather than
  // caching a QuerySlot* across the macro (Vec column reallocs would
  // invalidate it).
  DB_QUERY_GUARD(s, QUERY_MODULE_AST, stable_mod,
                 db_locate_slot(s, QUERY_MODULE_AST, stable_mod)->fingerprint,
                 FINGERPRINT_NONE, FINGERPRINT_NONE);

  // Reparse hygiene. The durable AST Vecs, top_level_index,
  // node_to_decl, line_starts and trivia columns are malloc-backed and
  // grow without bound; free the PRIOR parse's buffers before the
  // per-module arena is reset. The ASTStore STRUCT lives in that arena
  // (dangling after reset), so read it and free its Vecs first.
  // vec_free is safe/idempotent on the zeroed slots of a first parse.
  ASTStore *prev = *(ASTStore **)vec_get(&s->modules.asts, mod.idx);
  if (prev) {
    vec_free(&prev->kinds);
    vec_free(&prev->main_tokens);
    vec_free(&prev->data);
    vec_free(&prev->extra);
  }
  vec_free((Vec *)vec_get(&s->modules.top_level_indices, mod.idx));
  vec_free((Vec *)vec_get(&s->modules.node_to_decls, mod.idx));
  vec_free((Vec *)vec_get(&s->modules.line_starts, mod.idx));
  vec_free((Vec *)vec_get(&s->modules.trivia_tokens, mod.idx));
  vec_free((Vec *)vec_get(&s->modules.trivia_offsets, mod.idx));

  // Per-module isolation: reclaim the prior parse's ASTStore struct +
  // flattened node-data block in O(1). Other modules' arenas untouched
  // (this is the incremental-reparse unit).
  Arena *ma = (Arena *)vec_get(&s->modules.arenas, mod.idx);
  arena_reset(ma);

  // 1. Source bytes.
  FileId file_id = *(FileId *)vec_get(&s->modules.files, mod.idx);
  uint32_t src_idx = file_id_local(file_id);
  const char *source = *(const char **)vec_get(&s->sources.texts, src_idx);
  size_t source_len = *(uint32_t *)vec_get(&s->sources.text_lens, src_idx);

  // 2. Lex. raw_tokens is transient; line_starts persists in its column.
  Vec raw_tokens, line_starts;
  vec_init(&raw_tokens, sizeof(Token));
  vec_init(&line_starts, sizeof(uint32_t));
  lex(source, source_len, &s->strings, &raw_tokens, &line_starts);
  *(Vec *)vec_get(&s->modules.line_starts, mod.idx) = line_starts;

  // 3. Layout. real_tokens transient; trivia persists in its columns.
  Vec real_tokens, trivia_tokens, trivia_offsets;
  vec_init(&real_tokens, sizeof(Token));
  vec_init(&trivia_tokens, sizeof(Token));
  vec_init(&trivia_offsets, sizeof(uint32_t));
  layout(&raw_tokens, line_starts.data, line_starts.count, &real_tokens,
         &trivia_tokens, &trivia_offsets);
  *(Vec *)vec_get(&s->modules.trivia_tokens, mod.idx) = trivia_tokens;
  *(Vec *)vec_get(&s->modules.trivia_offsets, mod.idx) = trivia_offsets;

  // 4. Parse. parse_module is the query body proper: it writes the
  //    ASTStore (in ma), span/node_data block, top_level_index, and
  //    node_to_decl directly into this module's columns, and emits
  //    diagnostics via db_diag_* (slot-keyed to QUERY_MODULE_AST).
  parse_module(s, mod, &real_tokens);

  // 5. Durable fingerprint over the ASTStore (drives early cutoff).
  ASTStore *ast = *(ASTStore **)vec_get(&s->modules.asts, mod.idx);
  Fingerprint f1 =
      db_fp_bytes(ast->kinds.data, ast->kinds.count * sizeof(AstNodeKind));
  Fingerprint f2 = db_fp_bytes(ast->main_tokens.data,
                               ast->main_tokens.count * sizeof(uint32_t));
  Fingerprint f3 =
      db_fp_bytes(ast->data.data, ast->data.count * sizeof(AstNodeData));
  Fingerprint f4 =
      db_fp_bytes(ast->extra.data, ast->extra.count * sizeof(uint32_t));
  Fingerprint final_fp =
      db_fp_combine(db_fp_combine(f1, f2), db_fp_combine(f3, f4));
  *(Fingerprint *)vec_get(&s->modules.durable_fps, mod.idx) = final_fp;

  vec_free(&raw_tokens);
  vec_free(&real_tokens);

  db_query_succeed(s, QUERY_MODULE_AST, stable_mod, final_fp);
  return final_fp;
}
