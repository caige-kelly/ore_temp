// File readers — accessors for the file-table SoA columns.

#include "../db.h"
#include "../../syntax/syntax.h"  // SyntaxNode navigation (for db_get_node_span)

SourceId db_get_file_source(struct db *s, FileId fid) {
  if (!file_id_valid(fid))
    return SOURCE_ID_NONE;
  uint32_t local = file_id_local(fid);
  if (local >= s->files.source_id.count)
    return SOURCE_ID_NONE;
  return *(SourceId *)vec_get(&s->files.source_id, local);
}

NamespaceId db_get_file_namespace(struct db *s, FileId fid) {
  if (!file_id_valid(fid))
    return NAMESPACE_ID_NONE;
  uint32_t local = file_id_local(fid);
  if (local >= s->files.module_id.count)
    return NAMESPACE_ID_NONE;
  return *(NamespaceId *)vec_get(&s->files.module_id, local);
}

// Source → File. O(1) via the file_by_source HashMap (populated by
// db_create_file). 1:1 source-to-file today; N:1 future would
// promote the value to a Vec<FileId> offset.
FileId db_lookup_file_by_source(struct db *s, SourceId src) {
  if (!source_id_valid(src))
    return FILE_ID_NONE;
  void *v = hashmap_get(&s->file_by_source, (uint64_t)src.idx);
  uint32_t idx = (uint32_t)(uintptr_t)v;
  if (idx == 0)
    return FILE_ID_NONE;
  return file_id_make_physical(idx);
}

// Phase 8 eviction gate — return true if the file's backing source
// has been evicted (FS watcher reported the file as deleted). After
// eviction the per-file arena and source text are freed; readers
// must bail before any pointer deref.
static inline bool file_is_evicted(struct db *s, uint32_t local) {
  if (local >= s->files.source_id.count)
    return true;
  SourceId sid = *(SourceId *)vec_get(&s->files.source_id, local);
  return db_get_source_evicted(s, sid);
}

// Span for a SyntaxNode. With trivia in the green tree, the node's
// text_range may include leading trivia (whitespace/comments before
// the first significant token). For now we return the raw range; if
// diag UX needs tighter anchoring, swap to a trivia-trimmed helper
// in src/syntax/text.c.
TinySpan db_get_node_span(struct db *s, FileId fid, SyntaxNode *node) {
  if (!file_id_valid(fid) || node == NULL)
    return TINYSPAN_NONE;
  uint32_t local = file_id_local(fid);
  if (file_is_evicted(s, local))
    return TINYSPAN_NONE;
  TextRange r = syntax_node_text_range(node);
  return span_make((uint16_t)local, r.start, r.length);
}

