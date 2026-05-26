// Position / cursor helpers — used by LSP and any future CLI
// `--at-position` query. No caching; pure structural lookups.
//
// LSP convention is 0-indexed (line, character). UTF-8 source assumed;
// UTF-16 conversion (LSP default encoding) is deferred until non-ASCII
// source actually shows up.

#include "../db.h"
#include "../../syntax/syntax.h"

#include <stdint.h>

uint32_t db_byte_offset_at(struct db *s, FileId fid, uint32_t line0,
                           uint32_t char0) {
  if (!file_id_valid(fid))
    return UINT32_MAX;
  uint32_t local = file_id_local(fid);
  if (local >= s->files.source_id.count)
    return UINT32_MAX;

  // Phase 8 — eviction gate. After eviction the per-file arena
  // (which owns line_starts->data) is freed; we must not deref.
  SourceId sid = *(SourceId *)vec_get(&s->files.source_id, local);
  if (db_get_source_evicted(s, sid))
    return UINT32_MAX;

  FileArray *line_starts = (FileArray *)vec_get(&s->files.line_starts, local);
  if (!line_starts || !line_starts->data || line0 >= line_starts->count)
    return UINT32_MAX;

  const uint32_t *starts = (const uint32_t *)line_starts->data;
  uint32_t line_start = starts[line0];
  uint32_t line_end = (line0 + 1 < line_starts->count) ? starts[line0 + 1]
                                                       : line_start + char0 + 1;
  if (sid.idx < s->sources.text_lens.count) {
    uint32_t text_len = *(uint32_t *)vec_get(&s->sources.text_lens, sid.idx);
    if (line_end > text_len)
      line_end = text_len;
  }
  uint32_t off = line_start + char0;
  if (off > line_end)
    off = line_end;
  return off;
}

// Resolves to the innermost SyntaxNode whose absolute byte range
// contains `byte_offset`. Walks green tree from the root, descending
// through the unique child whose range covers the offset until no
// child does. RETURNS_OWNED — caller releases via syntax_node_release.
//
// Returns NULL for invalid file, evicted source, no parsed tree, or
// an offset outside the file's text range.
SyntaxNode *db_node_at_offset(struct db *s, FileId fid, uint32_t byte_offset) {
  if (!file_id_valid(fid))
    return NULL;
  uint32_t local = file_id_local(fid);
  if (local >= s->files.green_roots.count)
    return NULL;

  // Phase 8 — eviction gate.
  if (local < s->files.source_id.count) {
    SourceId sid = *(SourceId *)vec_get(&s->files.source_id, local);
    if (db_get_source_evicted(s, sid))
      return NULL;
  }

  GreenNode *root_green = *(GreenNode **)vec_get(&s->files.green_roots, local);
  if (!root_green)
    return NULL;

  // Build a transient SyntaxTree wrapper around the green root so we
  // can navigate. The wrapper retains green; we free it at function
  // end. syntax_tree_root returns a +1 ref on the root SyntaxNode.
  SyntaxTree *tree = syntax_tree_new(root_green);
  SyntaxNode *cursor = syntax_tree_root(tree);

  // Bail if the offset is outside the file.
  TextRange cur_range = syntax_node_text_range(cursor);
  if (byte_offset < cur_range.start ||
      byte_offset >= cur_range.start + cur_range.length) {
    syntax_node_release(cursor);
    syntax_tree_free(tree);
    return NULL;
  }

  // Descend: at each level, find the child whose absolute range
  // contains `byte_offset`. Stop when no node-child covers it.
  for (;;) {
    uint32_t count = syntax_node_num_children(cursor);
    SyntaxNode *next = NULL;
    for (uint32_t i = 0; i < count; i++) {
      GreenElement g = green_node_child(syntax_node_green(cursor), i);
      if (g.kind != GREEN_ELEM_NODE)
        continue;
      // Compute child's absolute range from cursor's offset + rel_offset.
      uint32_t child_start = cur_range.start + g.rel_offset;
      uint32_t child_end = child_start + green_node_text_len(g.node);
      if (byte_offset >= child_start && byte_offset < child_end) {
        next = syntax_node_child(cursor, i);
        break;
      }
    }
    if (!next)
      break;
    syntax_node_release(cursor);
    cursor = next;
    cur_range = syntax_node_text_range(cursor);
  }
  syntax_tree_free(tree);
  return cursor; // caller owns +1
}
