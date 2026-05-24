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
  // writes the real+synthetic stream directly into real_tokens.
  // line_starts is grown in place by the cursor as it scans, so it is
  // persisted into its column AFTER the fused pass (a by-value copy
  // taken before would capture a stale count / pre-realloc data ptr).
  Vec line_starts;
  vec_init(&line_starts, sizeof(uint32_t));

  Vec real_tokens, trivia_tokens, trivia_offsets;
  vec_init(&real_tokens, sizeof(Token));
  vec_init(&trivia_tokens, sizeof(TriviaSpan)); // zero-copy trivia (B2)
  vec_init(&trivia_offsets, sizeof(uint32_t));
  // Tier A reserve, now on the single token Vec (raw_tokens is gone).
  // Dense Ore averages ~3 bytes/token; real+synthetic, trivia spans
  // and offsets are each ~token-count bounded. Pure capacity hint to
  // kill Vec-doubling reallocs (macOS routes large realloc through a
  // kernel mach_vm_copy) — behavior identical.
  size_t est = source_len / 3 + 16;
  vec_reserve(&real_tokens, est);
  vec_reserve(&trivia_tokens, est);
  vec_reserve(&trivia_offsets, est);

  LexCursor lc;
  lex_begin(&lc, source, (uint32_t)source_len, &s->strings, &line_starts);
  layout_stream(&lc, &line_starts, &real_tokens, &trivia_tokens,
                &trivia_offsets);

  // Persist line_starts + trivia into the per-file arena as flat
  // FileArrays, then free the lexer's scratch Vecs.
  *(FileArray *)vec_get(&s->files.line_starts, f) = file_array_copy(
      ma, line_starts.data, (uint32_t)line_starts.count, sizeof(uint32_t));
  *(FileArray *)vec_get(&s->files.trivia_tokens, f) =
      file_array_copy(ma, trivia_tokens.data, (uint32_t)trivia_tokens.count,
                      sizeof(TriviaSpan));
  *(FileArray *)vec_get(&s->files.trivia_offsets, f) =
      file_array_copy(ma, trivia_offsets.data, (uint32_t)trivia_offsets.count,
                      sizeof(uint32_t));
  vec_free(&line_starts);
  vec_free(&trivia_tokens);
  vec_free(&trivia_offsets);

  // 4. Parse. parse_file is the query body proper: it writes the
  //    ASTStore (in ma), span/node_data block, and top_level_index
  //    directly into this file's columns, and emits diagnostics via
  //    db_diag_* (slot-keyed to QUERY_FILE_AST).
  parse_file(s, fid, &real_tokens);

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

  vec_free(&real_tokens);

  db_query_succeed(s, QUERY_FILE_AST, (uint64_t)fid.idx, final_fp);
  return final_fp;
}
