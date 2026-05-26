#include "ast.h"
#include "../../lexer/layout.h"
#include "../../lexer/lexer.h"
#include "../../parser_new/parser.h"   // parse_file_green, ParseError
#include "../../syntax/syntax.h"       // GreenNode, green_node_release
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

  // Reparse hygiene. Drop the prior parse's green tree (the NodeCache
  // still holds its own +1 on each interned node, so structural
  // sharing across reparses survives). Clear the sparse node→def map
  // — QUERY_NODE_TO_DECL repopulates it.
  struct GreenNode **groot_slot =
      (struct GreenNode **)vec_get(&s->files.green_roots, f);
  if (*groot_slot) {
    green_node_release(*groot_slot);
    *groot_slot = NULL;
  }
  hashmap_clear((HashMap *)vec_get(&s->files.node_to_def, f));

  // Per-file isolation: reclaim the prior parse's FileArray bodies
  // (line_starts, tokens, top_level_indices, imports). Other files'
  // arenas untouched — this is the incremental-reparse unit.
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

  // 2-3. Fused lex+layout. No intermediate raw-token array: the cursor
  // pulls one raw token at a time and the layout driver writes a single
  // document-order stream (trivia + virtual layout + real tokens, all
  // in source order, each carrying its full SK_* kind) into `tokens`.
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

  // 4. Parse into a green tree. parser_new consumes the unified token
  //    stream directly (trivia threaded into the tree as leading
  //    children of the next non-trivia token); no pre-filter needed.
  Vec errors;
  vec_init(&errors, sizeof(ParseError));
  GreenNode *root = parse_file_green(&tokens, source, s->node_cache, &errors);
  vec_free(&tokens);

  // Stash the root for the workspace's lifetime. The cache holds an
  // independent +1 so structural sharing survives subsequent reparses;
  // this slot owns the file's "active" handle.
  *groot_slot = root;

  // Parse errors: drained but not yet emitted. Phase 4 will wire them
  // through db_emit once AstSpan is reshaped to anchor on
  // SyntaxNodePtr; until then the old AstNodeId-shaped anchor is
  // incompatible with parser_new's output. The errors Vec is freed
  // here — the strings are static literals owned by parser_new.
  vec_free(&errors);

  // 5. Durable fingerprint. Phase 3 placeholder: hash the unified
  //    token stream's bytes (approximates "did the file content
  //    change"). Phase 4 replaces this with the memoized
  //    content-hash field on the root GreenNode (trivia-invariant,
  //    sibling-stable — what test-decl-incremental actually requires).
  FileArray *tok_fa = (FileArray *)vec_get(&s->files.tokens, f);
  Fingerprint final_fp =
      db_fp_bytes(tok_fa->data, (size_t)tok_fa->count * sizeof(Token));

  db_query_succeed(s, QUERY_FILE_AST, (uint64_t)fid.idx, final_fp);
  return final_fp;
}
