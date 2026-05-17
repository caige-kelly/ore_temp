#include "parser.h"

#include "../db/diag/diag.h"

#include <string.h>

// -----------------------------------------------------------------------------
// Cursor Primitives
// -----------------------------------------------------------------------------

bool p_is_eof(const Parser *p) { return p_peek(p) == TK_EOF; }

const Token *p_current(const Parser *p) {
  // Typed unchecked read: the index is range-clamped right here, so
  // vec_get's bounds check + call overhead is pure redundancy on this
  // hot path (every p_peek/p_advance/p_consume/p_is_eof routes here).
  const Token *toks = (const Token *)p->tokens->data;
  if (p->pos >= p->tokens->count) {
    if (p->tokens->count == 0)
      return NULL;
    return &toks[p->tokens->count - 1];
  }
  return &toks[p->pos];
}

TokenKind p_peek(const Parser *p) {
  const Token *t = p_current(p);
  return t ? t->kind : TK_EOF;
}

TokenKind p_peek_at(const Parser *p, uint32_t offset) {
  uint32_t idx = p->pos + offset;
  if (idx >= p->tokens->count) {
    if (p->tokens->count == 0)
      return TK_EOF;
    const Token *last = vec_get((Vec *)p->tokens, p->tokens->count - 1);
    return last->kind;
  }
  const Token *t = vec_get((Vec *)p->tokens, idx);
  return t->kind;
}

const Token *p_advance(Parser *p) {
  const Token *t = p_current(p);
  if (!p_is_eof(p)) {
    p->pos++;
  }
  return t;
}

bool p_match(Parser *p, TokenKind kind) {
  if (p_peek(p) == kind) {
    p_advance(p);
    return true;
  }
  return false;
}

const Token *p_consume(Parser *p, TokenKind kind, const char *err_msg) {
  if (p_peek(p) == kind) {
    return p_advance(p);
  }
  p_error(p, err_msg);
  return NULL;
}

void p_error(Parser *p, const char *msg) {
  // Slot-keyed diagnostic. The parser runs inside QUERY_FILE_AST's
  // body, so db_diag_error attributes this to that file's slot
  // (memoized/invalidated with the parse; LSP reads it per-file).
  const Token *t = p_current(p);
  uint32_t start = t ? t->start : 0u;
  uint32_t end = t ? t->byte_end : 0u;
  TinySpan span = span_make_range((uint16_t)p->file.idx, start, end);
  db_diag_error(p->s, span, msg);
}

// -----------------------------------------------------------------------------
// Node Construction
// -----------------------------------------------------------------------------

AstNodeId p_push_node(Parser *p, AstNodeKind kind, uint32_t main_token,
                      AstNodeData data, TinySpan span) {
  AstNodeId id = ast_push_node(p->ast, kind, main_token, data);

  // span_map parallels the ASTStore by node index. Stamp the file_id
  // defensively in case the caller built `span` without it.
  span = span_with_file(span, (uint16_t)p->file.idx);
  *(TinySpan *)vec_push_slot(&p->span_map) = span;

  return id;
}

// -----------------------------------------------------------------------------
// Core Driver — QUERY_FILE_AST body
// -----------------------------------------------------------------------------

void parse_file(struct db *s, FileId fid, const Vec *tokens) {
  uint32_t f = file_id_local(fid);
  Arena *ma = (Arena *)vec_get(&s->files.arenas, f);

  // The ASTStore STRUCT lives in the per-file arena (fixed size,
  // reclaimed by arena_reset on reparse). Its Vecs are malloc-backed
  // and growable (ast_store_init) — freed explicitly by the query
  // body before the next arena_reset.
  ASTStore *ast = arena_alloc(ma, sizeof(ASTStore));
  ast_store_init(ast, ma, 0);

  Parser p = {
      .s = s,
      .file = fid,
      .tokens = tokens,
      .pos = 0,
      .ast = ast,
      .parsing_type = false,
  };

  // All malloc-backed (growable, no caps). span_map + scratch are
  // transient — vec_free'd at the end of this function. top_level_index
  // + node_to_decl persist into db columns; the query body vec_free's
  // the prior ones on reparse before calling here.
  vec_init(&p.span_map, sizeof(TinySpan));
  vec_init(&p.scratch, sizeof(uint32_t));
  vec_init(&p.top_level_index, sizeof(TopLevelEntry));
  vec_init(&p.node_to_decl, sizeof(DefId));

  // Sentinels keep span_map / node_to_decl index-aligned with the
  // ASTStore's node 0 (AST_ERROR sentinel pushed by ast_store_init).
  TinySpan dummy_span = {0};
  vec_push(&p.span_map, &dummy_span);
  DefId dummy_def = {0};
  vec_push(&p.node_to_decl, &dummy_def);

  extern void parse_top_level_decls(Parser * p);
  parse_top_level_decls(&p);

  // Commit parser outputs to the file's columns.
  *(void **)vec_get(&s->files.asts, f) = p.ast;
  *(Vec *)vec_get(&s->files.top_level_indices, f) = p.top_level_index;
  *(Vec *)vec_get(&s->files.node_to_decls, f) = p.node_to_decl;

  // Flatten span_map (+ zeroed parent/type maps — later passes fill
  // those) into one contiguous ModuleNodeData block in the per-file
  // arena. node_count includes the sentinel at index 0.
  uint32_t node_count = (uint32_t)p.span_map.count;
  void *block = arena_alloc(ma, (size_t)node_count * 16);
  TinySpan *spans = (TinySpan *)block;
  AstNodeId *parents = (AstNodeId *)((uint8_t *)block + (size_t)node_count * 8);
  uint32_t *types = (uint32_t *)((uint8_t *)parents + (size_t)node_count * 4);

  if (node_count > 0 && p.span_map.data) {
    memcpy(spans, p.span_map.data, (size_t)node_count * sizeof(TinySpan));
  } else if (node_count > 0) {
    memset(spans, 0, (size_t)node_count * sizeof(TinySpan));
  }
  if (node_count > 0) {
    memset(parents, 0, (size_t)node_count * sizeof(AstNodeId));
    memset(types, 0, (size_t)node_count * sizeof(uint32_t));
  }

  ModuleNodeData *nd =
      (ModuleNodeData *)vec_get(&s->files.node_data, f);
  nd->spans = spans;
  nd->parents = parents;
  nd->types = (IpIndex *)types;
  *(uint32_t *)vec_get(&s->files.node_counts, f) = node_count;

  // Transient malloc buffers: span_map was just flattened into the
  // arena block; scratch is parse-local. Free them now. (ast Vecs +
  // top_level_index + node_to_decl persist into columns and are freed
  // by the query body on the NEXT reparse, before arena_reset.)
  vec_free(&p.span_map);
  vec_free(&p.scratch);
}
