// Position / cursor helpers — used by LSP and any future CLI
// `--at-position` query. No caching; pure structural lookups against
// db.files.line_starts and db.files.node_data.spans.
//
// LSP convention is 0-indexed (line, character). UTF-8 source assumed;
// UTF-16 conversion (LSP default encoding) is deferred until non-ASCII
// source actually shows up.

#include "../db.h"

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

AstNodeId db_node_at_offset(struct db *s, FileId fid, uint32_t byte_offset) {
  if (!file_id_valid(fid))
    return AST_NODE_ID_NONE;
  uint32_t local = file_id_local(fid);
  if (local >= s->files.node_data.count || local >= s->files.node_counts.count)
    return AST_NODE_ID_NONE;

  // Phase 8 — eviction gate. nd->spans points into the per-file arena;
  // after eviction it's freed.
  if (local < s->files.source_id.count) {
    SourceId sid = *(SourceId *)vec_get(&s->files.source_id, local);
    if (db_get_source_evicted(s, sid))
      return AST_NODE_ID_NONE;
  }

  FileNodeData *nd = (FileNodeData *)vec_get(&s->files.node_data, local);
  if (!nd || !nd->spans)
    return AST_NODE_ID_NONE;
  uint32_t n = *(uint32_t *)vec_get(&s->files.node_counts, local);
  if (n == 0)
    return AST_NODE_ID_NONE;

  uint16_t expected_file = (uint16_t)local;

  // Innermost containing span wins. Linear scan over the file's full
  // span array (~5 µs per 50KB at modern memory bandwidth — fine for
  // LSP rates). The naive sort-by-start binary search isn't sound
  // here: the parser pushes nodes in post-order (children before
  // parents), so a parent and a child with the same span_start aren't
  // ordered the way an interval tree would expect. If this ever shows
  // up in a profile, the right fix is a per-file interval tree built
  // at parse time, not a simpler sorted index.
  uint32_t best = 0;
  uint32_t best_len = UINT32_MAX;
  for (uint32_t i = 1; i < n; i++) {
    TinySpan sp = nd->spans[i];
    if (span_file(sp) != expected_file)
      continue;
    uint32_t start = span_start(sp);
    uint32_t end = span_end(sp);
    if (byte_offset < start || byte_offset >= end)
      continue;
    uint32_t len = end - start;
    if (len < best_len) {
      best_len = len;
      best = i;
    }
  }
  return (AstNodeId){.idx = best};
}
