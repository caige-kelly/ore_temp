#include "ast.h"
#include "../../lexer/layout.h"
#include "../../lexer/lexer.h"
#include "../../parser/ast.h" // ASTStore (for fingerprinting)
#include "../../parser/parser.h"
#include "invalidate.h"

#include <string.h>

// Copy `count` elements of `elem_size` bytes from a scratch buffer into
// the file's arena, returning a FileArray view. The arena was reset at
// the top of the reparse, so the copy survives until the next reparse.
static FileArray file_array_copy(Arena *arena, const void *src, uint32_t count,
                                 size_t elem_size) {
  FileArray fa = {.data = NULL, .count = count};
  if (count) {
    fa.data = arena_alloc_raw(arena, (size_t)count * elem_size);
    memcpy(fa.data, src, (size_t)count * elem_size);
  }
  return fa;
}

Fingerprint db_query_file_ast(struct db *s, FileId fid) {
  uint32_t f = file_id_local(fid);
  // DB_QUERY_GUARD evaluates the on_cached expression only when the
  // begin returns CACHED; we re-locate inside that branch rather than
  // caching a QuerySlot* across the macro (Vec column reallocs would
  // invalidate it).
  DB_QUERY_GUARD(
      s, QUERY_FILE_AST, (uint64_t)fid.idx,
      db_locate_slot(s, QUERY_FILE_AST, (uint64_t)fid.idx)->fingerprint,
      FINGERPRINT_NONE, FINGERPRINT_NONE);

  // Reparse hygiene. The durable AST Vecs, top_level_index, line_starts
  // and trivia columns are malloc-backed and grow without bound; free
  // the PRIOR parse's buffers before the per-file arena is reset. The
  // ASTStore STRUCT lives in that arena (dangling after reset), so read
  // it and free its Vecs first. vec_free is safe/idempotent on the
  // zeroed slots of a first parse.
  ASTStore *prev = *(ASTStore **)vec_get(&s->files.asts, f);
  if (prev) {
    vec_free(&prev->kinds);
    vec_free(&prev->main_tokens);
    vec_free(&prev->data);
    vec_free(&prev->extra);
  }
  // top_level_indices / line_starts / trivia_* are FileArrays whose data
  // lives in this file's arena — reclaimed by the arena_reset just below.

  // Per-file isolation: reclaim the prior parse's ASTStore struct +
  // flattened node-data block in O(1). Other files' arenas untouched
  // (this is the incremental-reparse unit).
  Arena *ma = (Arena *)vec_get(&s->files.arenas, f);
  arena_reset(ma);

  // 1. Source bytes — via the file's explicit source_id back-ref.
  SourceId src = *(SourceId *)vec_get(&s->files.source_id, f);
  uint32_t src_idx = src.idx;
  const char *source = *(const char **)vec_get(&s->sources.texts, src_idx);
  size_t source_len = *(uint32_t *)vec_get(&s->sources.text_lens, src_idx);

  // This parse's only input is the source text — declare its
  // durability so the slot inherits it (workspace=LOW, library=HIGH)
  // and the durability fast-path can early-cut dependents correctly.
  Durability sdur = *(Durability *)vec_get(&s->sources.durability, src_idx);
  db_query_note_input_durability(s, (uint8_t)sdur);

  // 2-3. Fused lex+layout (Tier C). No intermediate raw-token array:
  // the cursor pulls one raw token at a time and the layout driver
  // writes a single document-order stream (trivia + virtual layout +
  // real tokens, all in source order, each carrying its full SK_* kind)
  // into `tokens`.
  Vec line_starts;
  vec_init(&line_starts, sizeof(uint32_t));

  Vec tokens;
  vec_init(&tokens, sizeof(Token));
  size_t est = source_len / 3 + 16;
  vec_reserve(&tokens, est);

  LexCursor lc;
  lex_begin(&lc, source, (uint32_t)source_len, &s->strings, &line_starts);
  layout_stream(&lc, &line_starts, &tokens);

  // Persist line_starts + the unified token stream into the per-file
  // arena as flat FileArrays, then free the lexer's scratch Vecs.
  *(FileArray *)vec_get(&s->files.line_starts, f) = file_array_copy(
      ma, line_starts.data, (uint32_t)line_starts.count, sizeof(uint32_t));
  *(FileArray *)vec_get(&s->files.tokens, f) =
      file_array_copy(ma, tokens.data, (uint32_t)tokens.count, sizeof(Token));
  vec_free(&line_starts);

  // 4. Parse. The current flat-AST parser consumes only non-trivia
  //    tokens (with synthetics inlined). Build a filtered view from
  //    the unified stream and hand it to parse_file. When Phase A.1
  //    swaps the parser to the green builder, this intermediate Vec
  //    goes away — the parser will skip trivia inline via a cursor.
  Vec parse_tokens;
  vec_init(&parse_tokens, sizeof(Token));
  vec_reserve(&parse_tokens, tokens.count);
  for (size_t i = 0; i < tokens.count; i++) {
    Token *t = (Token *)vec_get(&tokens, i);
    if (!token_is_trivia(t->kind))
      *(Token *)vec_push_slot(&parse_tokens) = *t;
  }
  vec_free(&tokens);

  parse_file(s, fid, &parse_tokens);

  // 5. Durable fingerprint over the ASTStore (drives early cutoff).
  ASTStore *ast = *(ASTStore **)vec_get(&s->files.asts, f);
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

  vec_free(&parse_tokens);

  db_query_succeed(s, QUERY_FILE_AST, (uint64_t)fid.idx, final_fp);
  return final_fp;
}
