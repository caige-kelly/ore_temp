#include "ast.h"
#include "../../ast/ast_decl.h"        // ConstDef / VarDef wrappers
#include "../../lexer/layout.h"
#include "../../lexer/lexer.h"
#include "../../parser/syntax_kind.h"   // SK_* constants
#include "../../parser_new/parser.h"   // parse_file_green, ParseError
#include "../../support/data_structure/stringpool.h"
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

// Recognize a contextual-modifier IDENT token and OR its bit into meta.
// `pub`, `pvt` set the visibility two-bit field; the rest are 1-bit flags
// (see DefMeta in db.h).
static void absorb_modifier(struct db *s, SyntaxKind tk, StrId tok_str,
                            DefMeta *meta) {
  if (tk != SK_IDENT)
    return;
  if (tok_str.idx == s->names.PUB.idx) {
    *meta = (*meta & ~META_VIS_MASK) | VIS_PUBLIC;
  } else if (tok_str.idx == s->names.PVT.idx) {
    *meta = (*meta & ~META_VIS_MASK) | VIS_PRIVATE;
  } else if (tok_str.idx == s->names.ABSTRACT.idx) {
    *meta |= META_ABSTRACT;
  } else if (tok_str.idx == s->names.NAMED.idx) {
    *meta |= META_NAMED;
  } else if (tok_str.idx == s->names.SCOPED.idx) {
    *meta |= META_SCOPED;
  } else if (tok_str.idx == s->names.LINEAR.idx) {
    *meta |= META_LINEAR;
  } else if (tok_str.idx == s->names.DISTINCT.idx) {
    *meta |= META_DISTINCT;
  }
}

// Extract (name, meta) for a top-level decl wrapper.
// - Name comes from the wrapper's typed accessor (ConstDef_name /
//   VarDef_name) — clean wrapper-API usage.
// - Modifier idents (pub/pvt/abstract/named/scoped/linear/distinct)
//   appear as inline IDENT tokens BEFORE the LHS. These aren't a
//   structured concept in the grammar (no per-modifier SyntaxKind),
//   so the only correct read is a raw token walk stopping at the
//   bind operator. Documented as the legitimate raw-navigation
//   exception.
//
// Returns false if the wrapper has no recognizable name — in that
// case the entry is skipped from top_level_indices.
static bool extract_top_level_meta(struct db *s, SyntaxNode *wrapper,
                                   StrId *out_name, DefMeta *out_meta) {
  SyntaxKind wk = syntax_node_kind(wrapper);
  StrId name = (StrId){0};
  SyntaxToken *name_tok = NULL;
  if (wk == SK_CONST_DECL) {
    ConstDef c;
    if (ConstDef_cast(wrapper, &c)) name_tok = ConstDef_name(&c);
  } else if (wk == SK_VAR_DECL) {
    VarDef v;
    if (VarDef_cast(wrapper, &v)) name_tok = VarDef_name(&v);
  }
  if (name_tok) {
    const char *txt = syntax_token_text(name_tok);
    uint32_t len = syntax_token_text_range(name_tok).length;
    name = pool_intern(&s->strings, txt, len);
    syntax_token_release(name_tok);
  }
  if (name.idx == 0)
    return false;

  // Raw scan for contextual modifier tokens. Stops at the bind
  // operator (::, :=). No typed wrapper applies — these aren't named
  // children of the wrapper; they're inline IDENT tokens.
  DefMeta meta = 0;
  uint32_t count = syntax_node_num_children(wrapper);
  for (uint32_t i = 0; i < count; i++) {
    GreenElement g = green_node_child(syntax_node_green(wrapper), i);
    if (g.kind != GREEN_ELEM_TOKEN)
      continue;
    SyntaxKind tk = green_token_kind(g.token);
    if (tk == SK_COLON_COLON || tk == SK_COLON_EQ)
      break;
    if (tk == SK_IDENT) {
      const char *txt = green_token_text(g.token);
      uint32_t len = green_token_text_len(g.token);
      StrId tok_str = pool_lookup(&s->strings, txt, len);
      absorb_modifier(s, tk, tok_str, &meta);
    }
  }

  *out_name = name;
  *out_meta = meta;
  return true;
}

// Walk SK_SOURCE_FILE's direct node-children and emit a TopLevelEntry
// for every recognizable top-level decl wrapper (SK_CONST_DECL,
// SK_VAR_DECL, SK_DESTRUCTURE_DECL). DESTRUCTURE binds are skipped from
// the index — they don't have a single (name, def) anchor.
static void build_top_level_index(struct db *s, GreenNode *root,
                                  Arena *arena, FileArray *out) {
  if (!root) {
    *out = (FileArray){.data = NULL, .count = 0};
    return;
  }
  SyntaxTree *tree = syntax_tree_new(root);
  SyntaxNode *root_red = syntax_tree_root(tree);

  // Pass 1: count matching decl wrappers.
  uint32_t n = 0;
  uint32_t top_count = syntax_node_num_children(root_red);
  for (uint32_t i = 0; i < top_count; i++) {
    GreenElement g = green_node_child(syntax_node_green(root_red), i);
    if (g.kind != GREEN_ELEM_NODE)
      continue;
    SyntaxKind nk = green_node_kind(g.node);
    if (nk == SK_CONST_DECL || nk == SK_VAR_DECL)
      n++;
  }

  TopLevelEntry *entries = NULL;
  if (n > 0)
    entries = (TopLevelEntry *)arena_alloc_raw(
        arena, (size_t)n * sizeof(TopLevelEntry));

  // Pass 2: extract.
  uint32_t out_idx = 0;
  for (uint32_t i = 0; i < top_count && out_idx < n; i++) {
    GreenElement g = green_node_child(syntax_node_green(root_red), i);
    if (g.kind != GREEN_ELEM_NODE)
      continue;
    SyntaxKind nk = green_node_kind(g.node);
    if (nk != SK_CONST_DECL && nk != SK_VAR_DECL)
      continue;
    SyntaxNode *wrap = syntax_node_child(root_red, i);
    if (!wrap)
      continue;
    StrId name = (StrId){0};
    DefMeta meta = 0;
    if (extract_top_level_meta(s, wrap, &name, &meta)) {
      entries[out_idx].name = name;
      entries[out_idx].node_ptr = syntax_node_ptr_new(wrap);
      entries[out_idx].meta = meta;
      out_idx++;
    }
    syntax_node_release(wrap);
  }
  syntax_node_release(root_red);
  syntax_tree_free(tree);

  out->data = entries;
  out->count = out_idx;
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

  // Build the top-level decl index from the green tree. Stored in the
  // per-file arena; reclaimed on the next arena_reset.
  FileArray *tli = (FileArray *)vec_get(&s->files.top_level_indices, f);
  build_top_level_index(s, root, ma, tli);

  // Parse errors: drained but not yet emitted (TinySpan-anchored diag
  // wiring is Phase 4 follow-up). Strings are static literals owned by
  // parser_new, so the Vec free below doesn't leak them.
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
