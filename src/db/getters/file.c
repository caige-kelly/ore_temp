// File readers — accessors for the file-table SoA columns.

#include "../db.h"
#include "../query/node_to_def.h" // db_query_node_to_def — for db_get_def_for_node
#include "../../parser/syntax_kind.h" // SK_SOURCE_FILE (moves to src/syntax in Phase 5)
#include "../../syntax/syntax.h"  // SyntaxNode + SyntaxNodePtr navigation

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

// Enclosing top-level DefId for a SyntaxNode. Walks
// syntax_node_parent up to the direct child of SK_SOURCE_FILE, then
// probes files.node_to_def[fid] (the sparse SyntaxNodePtr→DefId
// HashMap populated by QUERY_NODE_TO_DECL). Returns DEF_ID_NONE if
// the node isn't inside any classified top-level decl, or
// node_to_def hasn't been populated for this file.
DefId db_get_def_for_node(struct db *s, FileId fid, SyntaxNode *node) {
  if (!file_id_valid(fid) || node == NULL)
    return DEF_ID_NONE;
  uint32_t local = file_id_local(fid);
  if (file_is_evicted(s, local))
    return DEF_ID_NONE;
  // Ensure the file's sparse node→DefId index is current.
  db_query_node_to_def(s, fid);
  if (local >= s->files.node_to_def.count)
    return DEF_ID_NONE;
  HashMap *map = (HashMap *)vec_get(&s->files.node_to_def, local);

  // Walk parents until we hit a direct child of SK_SOURCE_FILE — that's
  // the top-level decl wrapper for our enclosing scope. Each step
  // retains the parent and releases the prior cursor, except for the
  // input `node` (which the caller still owns).
  SyntaxNode *cursor = node;
  bool cursor_owned = false;
  DefId result = DEF_ID_NONE;
  while (cursor) {
    SyntaxNode *parent = syntax_node_parent(cursor);
    if (parent == NULL) {
      // Reached the root with no SOURCE_FILE intermediate — file
      // structure is malformed or node belongs to a different tree.
      break;
    }
    if (syntax_node_kind(parent) == SK_SOURCE_FILE) {
      // cursor is the top-level decl wrapper. Hash its SyntaxNodePtr
      // and probe the sparse map.
      SyntaxNodePtr ptr = syntax_node_ptr_new(cursor);
      void *v = hashmap_get(map, syntax_node_ptr_hash(ptr));
      if (v != NULL) {
        result = (DefId){.idx = (uint32_t)(uintptr_t)v};
      }
      syntax_node_release(parent);
      break;
    }
    // Climb. Release the prior cursor if we owned it.
    if (cursor_owned)
      syntax_node_release(cursor);
    cursor = parent;
    cursor_owned = true;
  }
  if (cursor_owned && cursor != node)
    syntax_node_release(cursor);
  return result;
}
